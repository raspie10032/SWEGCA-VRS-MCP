"""Generate exact-session Codex hooks for live VRS ingress and routing."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import shlex
import sys


def command(python, module_root, state_dir):
    return ' '.join(shlex.quote(str(x)) for x in (
        'env', f'PYTHONPATH={module_root}', python, '-m',
        'swegca_vrs2.conversation_hooks', '--host', 'codex',
        '--state-dir', state_dir))


def handler(run, timeout=120):
    return {'type': 'command', 'command': run, 'timeout': timeout}


def config(python, module_root, state_dir):
    run = command(python, module_root, state_dir)
    hooks = {}
    for event in ('UserPromptSubmit', 'PostToolUse', 'Stop', 'PreCompact',
                  'PostCompact'):
        hooks[event] = [{'hooks': [handler(run)]}]
    hooks['Interrupt'] = [{'hooks': [handler(run, 3)]}]
    hooks['SessionStart'] = [{'matcher': 'startup|resume|clear|compact',
                              'hooks': [handler(run)]}]
    hooks['SessionEnd'] = [{'matcher': 'other', 'hooks': [handler(run, 3)]}]
    hooks['PreToolUse'] = [{
        'matcher': '^mcp__swegca_vrs__memory_', 'hooks': [handler(run)]}]
    return {'description': 'SWEGCA-VRS2 session ingress and exact session routing',
            'hooks': hooks}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--python', type=Path, default=Path(sys.executable))
    parser.add_argument('--module-root', type=Path,
                        default=Path(__file__).resolve().parents[1] / 'src')
    parser.add_argument('--state-dir', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    python, module_root, state = (path.expanduser().resolve() for path in
                                  (args.python, args.module_root, args.state_dir))
    if not python.is_file() or not (module_root / 'swegca_vrs2/session_capture.py').is_file():
        parser.error('python or source module unavailable')
    output = args.output.expanduser().resolve()
    if output.exists():
        parser.error(f'output already exists: {output}')
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(config(python, module_root, state),
                                 ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    output.chmod(0o600)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
