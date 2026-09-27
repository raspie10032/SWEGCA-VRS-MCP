#!/usr/bin/env python3
"""Install an opt-in VRS desktop entry. Does not launch or stop any process.

Only deployment plumbing lives here; all experience/decision logic is C++.
Existing installs and desktop entries are never overwritten.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import tempfile

BINS = ('swegca-codex-wrapper', 'swegca-desktop-host', 'swegca-app-server-proxy', 'swegca-vrs-mcp')


def desktop_argument(value):
    # Desktop Entry quoting, not shell quoting. Literal percent is doubled.
    value = value.replace('%', '%%')
    for char in ('\\', '"', '`', '$'):
        value = value.replace(char, '\\' + char)
    return '"' + value + '"'


def write_json(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n')
    path.chmod(0o600)


def install(args):
    repo = Path(__file__).resolve().parents[1]
    build = Path(args.build_dir).resolve(strict=True)
    backend = Path(args.backend).resolve(strict=True)
    prefix = Path(args.prefix).expanduser().absolute()
    entry = Path(args.desktop_file).expanduser().absolute()
    if prefix.exists() or prefix.is_symlink() or entry.exists() or entry.is_symlink():
        raise ValueError('installation destination already exists; no files overwritten')
    for executable in [backend, *(build / name for name in BINS)]:
        if not executable.is_file() or not os.access(executable, os.X_OK):
            raise ValueError('missing executable: ' + str(executable))
    template = Path(args.desktop_template).read_text()
    fields = {}; active = False
    for line in template.splitlines():
        if line.startswith('['):
            active = line == '[Desktop Entry]'
        elif active and '=' in line and not line.startswith('#'):
            key, value = line.split('=', 1); fields[key] = value
    if fields.get('Type') != 'Application' or not fields.get('Exec'):
        raise ValueError('template must contain an application Exec entry')
    if any(key in fields['Exec'] for key in ('CODEX_CLI_PATH', 'SWEGCA_DESKTOP_CONFIG')):
        raise ValueError('template already overrides the backend')
    config = json.loads((repo / 'examples/stdio-config.json').read_text())
    native_frame = 16 << 20
    config.update(memoryBytes='4000000000', frameBytes=str(native_frame * 6 + 65536), readLimit='33554432',
                  sessionBlockBytes='67108864', mainBlockBytes='8388608', cpuAffinity='6 7',
                  ioBytesPerSecond='625000000', storageBytes='500000000000', mergeWorkers='2',
                  mainIdentity=os.urandom(32).hex())
    proxy = {'memoryBytes': str(512 << 20), 'frameBytes': str(native_frame),
             'pendingRequests': '256', 'sessionCapacity': '128', 'seed': '7', 'step': '0',
             'instance': 'desktop-' + os.urandom(16).hex(),
             'connectionSession': {'session': 'transport', 'mode': 'ensure'}, 'sessions': []}
    wrapper = {'backend': str(backend), 'host': str(prefix / 'bin/swegca-desktop-host'),
               'proxy': str(prefix / 'bin/swegca-app-server-proxy'),
               'vrs': str(prefix / 'bin/swegca-vrs-mcp'), 'root': str(prefix / 'experience'),
               'resourceConfig': str(prefix / 'resources.json'),
               'proxyConfig': str(prefix / 'proxy.json'), 'mode': 'limited-ensure'}
    prefix.parent.mkdir(parents=True, exist_ok=True)
    entry.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.swegca-install-', dir=prefix.parent) as temporary:
        staging = Path(temporary) / 'payload'; staging.mkdir(mode=0o700)
        (staging / 'bin').mkdir(mode=0o700); (staging / 'experience').mkdir(mode=0o700)
        manifest = {'backend': str(backend), 'executables': {}}
        for name in BINS:
            destination = staging / 'bin' / name
            shutil.copyfile(build / name, destination); destination.chmod(0o700)
            with destination.open('rb') as executable:
                manifest['executables'][name] = hashlib.file_digest(executable, 'sha256').hexdigest()
        write_json(staging / 'resources.json', config); write_json(staging / 'proxy.json', proxy)
        write_json(staging / 'desktop.json', wrapper); write_json(staging / 'manifest.json', manifest)
        # A separate entry leaves the user's existing launcher and MIME handlers intact.
        fields = {key: value for key, value in fields.items()
                  if key not in ('Actions', 'MimeType', 'DBusActivatable') and not key.startswith(('Name[', 'Comment['))}
        fields['Name'] = 'Codex · SWEGCA VRS'
        fields['Comment'] = 'Start after fully closing the existing Codex desktop app'
        fields['Exec'] = '/usr/bin/env ' + desktop_argument('CODEX_CLI_PATH=' + str(prefix / 'bin/swegca-codex-wrapper')) + ' ' + desktop_argument('SWEGCA_DESKTOP_CONFIG=' + str(prefix / 'desktop.json')) + ' ' + fields['Exec']
        desktop = '[Desktop Entry]\n' + ''.join(key + '=' + value + '\n' for key, value in fields.items())
        # Exclusive creation prevents a concurrent install from replacing user files.
        os.mkdir(prefix, 0o700)
        for child in staging.iterdir():
            os.rename(child, prefix / child.name)
        with entry.open('x') as output:
            output.write(desktop)
        entry.chmod(0o644)
    print(json.dumps({'prefix': str(prefix), 'desktopEntry': str(entry), 'launched': False}))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', required=True)
    parser.add_argument('--backend', required=True)
    parser.add_argument('--prefix', required=True)
    parser.add_argument('--desktop-template', required=True)
    parser.add_argument('--desktop-file', required=True)
    install(parser.parse_args())
