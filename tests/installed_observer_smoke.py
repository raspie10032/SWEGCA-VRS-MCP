#!/usr/bin/env python3
"""Opt-in installed backend inventory/profile check; no threads or model calls.

Usage: python3 tests/installed_observer_smoke.py INSTALLED_PREFIX
For this verification process only, query the local observer without starting
other configured MCP connectors. Persistent user configuration is untouched.
"""
import json
import os
from pathlib import Path
import select
import subprocess
import sys
import tempfile
import time

prefix = Path(sys.argv[1]).resolve(strict=True)
resources = json.loads((prefix / 'resources.json').read_text())
config = json.loads((prefix / 'desktop.json').read_text())
# Keep the installed command/arguments as the source of truth.
values = dict(item.split('=', 1) for item in config['backendConfig'])
server = 'swegca_content_observer'
command = json.loads(values[f'mcp_servers.{server}.command'])
arguments = json.loads(values[f'mcp_servers.{server}.args'])
env_vars = json.loads(values.get(f'mcp_servers.{server}.env_vars', '[]'))
override = 'mcp_servers={' + server + '={command=' + json.dumps(command) + ',args=' + json.dumps(arguments) + ',env_vars=' + json.dumps(env_vars) + '}}'
env = dict(os.environ, SWEGCA_DESKTOP_CONFIG=str(prefix / 'desktop.json'))
roles = {'swegca-desktop-host', 'swegca-app-server-proxy', 'swegca-vrs-mcp', 'swegca-content-observer'}
profiles = {}


def scan():
    for proc in Path('/proc').iterdir():
        if not proc.name.isdigit():
            continue
        try:
            executable = (proc / 'exe').resolve(strict=True)
            if executable.parent != prefix / 'bin' or executable.name not in roles:
                continue
            group = next(line[3:] for line in (proc / 'cgroup').read_text().splitlines() if line.startswith('0::'))
            root = Path('/sys/fs/cgroup') / group.lstrip('/')
            cpus = next(line.split(':', 1)[1].strip() for line in (proc / 'status').read_text().splitlines() if line.startswith('Cpus_allowed_list:'))
            shared_owner = next((part.split(b'=', 1)[1].decode() for part in (proc / 'environ').read_bytes().split(b'\0')
                                 if part.startswith(b'SWEGCA_IO_OWNER=')), None)
            profiles[executable.name] = {'group': group, 'memoryMax': int((root / 'memory.max').read_text()),
                                        'swapMax': int((root / 'memory.swap.max').read_text()), 'cpus': cpus,
                                        'pid': proc.name, 'sharedOwner': shared_owner}
        except (OSError, RuntimeError, StopIteration):
            continue


with tempfile.TemporaryFile() as errors:
    process = subprocess.Popen([str(prefix / 'bin/swegca-codex-wrapper'), '-c', override,
                                'app-server', '--listen=stdio://'], stdin=subprocess.PIPE,
                               stdout=subprocess.PIPE, stderr=errors, env=env)

    def send(value):
        process.stdin.write(json.dumps(value).encode() + b'\n')
        process.stdin.flush()

    def receive(number):
        deadline = time.monotonic() + 25
        while time.monotonic() < deadline:
            scan()
            if not select.select([process.stdout], [], [], 0.1)[0]:
                continue
            raw = process.stdout.readline()
            if not raw:
                raise RuntimeError('backend ended before response')
            value = json.loads(raw)
            if value.get('id') == number:
                if 'error' in value:
                    raise RuntimeError('backend rejected inventory request')
                return value['result']
        raise TimeoutError('backend inventory response timeout')

    try:
        send({'id': 1, 'method': 'initialize', 'params': {
            'clientInfo': {'name': 'swegca_observer_inventory', 'version': '0.1'}}})
        receive(1)
        send({'method': 'initialized'})
        send({'id': 2, 'method': 'mcpServerStatus/list', 'params': {'detail': 'toolsAndAuthOnly'}})
        status = receive(2)
        scan()
        matches = [entry for entry in status.get('data', []) if entry.get('name') == server]
        assert len(matches) == 1 and 'observe_file_content_equality' in matches[0].get('tools', {})
        assert profiles.keys() == roles, 'some installed process profiles were not observed'
        assert len({value['group'] for value in profiles.values()}) == 1, 'observer escaped aggregate group'
        owner = profiles['swegca-vrs-mcp']['sharedOwner']
        assert owner and profiles['swegca-content-observer']['sharedOwner'] == owner
        owner_pid, descriptor = owner.split(':')
        assert owner_pid == profiles['swegca-desktop-host']['pid']
        assert 'memfd:swegca-transfer' in os.readlink(f'/proc/{owner_pid}/fd/{descriptor}')
        page = os.sysconf('SC_PAGESIZE')
        expected = int(resources['memoryBytes']) // page * page
        for value in profiles.values():
            assert value['memoryMax'] == expected and value['swapMax'] == 0 and value['cpus'] == '6-7'
        process.stdin.close()
        assert process.wait(timeout=20) == 0
        print(json.dumps({'observerRegistered': True, 'sameAggregateGroup': True, 'sameSharedTransfer': True,
                          'verifiedProcesses': sorted(profiles), 'memoryMax': expected,
                          'swapMax': 0, 'cpus': '6-7', 'modelCalls': 0, 'exitCode': 0}))
    finally:
        if not process.stdin.closed:
            process.stdin.close()
        if process.poll() is None:
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.terminate()
                process.wait(timeout=10)
