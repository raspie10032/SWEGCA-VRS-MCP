"""Generate Codex lifecycle hooks for live session VRS ingress and routing."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import shlex
import sys


def command(python, state_dir, *, module_root=None, server_name='swegca-vrs'):
    prefix = f'mcp__{server_name.replace("-", "_")}__memory_'
    values = []
    if module_root is not None:
        values.extend(('env', f'PYTHONPATH={Path(module_root).expanduser().resolve()}'))
    # Keep a virtual environment's executable path intact.  Resolving its
    # ``bin/python`` symlink selects the base interpreter and drops that
    # environment's site-packages, so the installed swegca_vrs2 module is no
    # longer importable when Codex invokes the generated hook.
    values.extend((str(Path(python).expanduser().absolute()), '-m',
        'swegca_vrs2.conversation_hooks', '--host', 'codex',
        '--state-dir', str(Path(state_dir).expanduser().resolve()),
        '--tool-prefix', prefix))
    return ' '.join(shlex.quote(value) for value in values)


def handler(run, timeout=120):
    return {'type': 'command', 'command': run, 'timeout': timeout}


def config(python, state_dir, *, module_root=None, server_name='swegca-vrs'):
    if not re.fullmatch(r'[A-Za-z0-9_-]{1,64}', server_name):
        raise ValueError('invalid_mcp_server_name')
    run = command(python, state_dir, module_root=module_root, server_name=server_name)
    hooks = {}
    for event in ('PostToolUse', 'Stop', 'PreCompact',
                  'PostCompact'):
        hooks[event] = [{'hooks': [handler(run)]}]
    # A command hook starts Python on every prompt. Use Codex's existing MCP
    # connection so user input reaches the resident VRS before model dispatch.
    hooks['UserPromptSubmit'] = [{'hooks': [{
        'type': 'mcp_tool', 'server': server_name, 'tool': 'memory_prompt',
        'input': {'session_id': '${session_id}', 'prompt': '${prompt}'},
        'timeout': 120}]}]
    hooks['Interrupt'] = [{'hooks': [handler(run, 3)]}]
    # Do not guess host lifecycle reasons. Every real start begins the tailer,
    # and every real end schedules the final stable capture and attachment.
    hooks['SessionStart'] = [{'hooks': [handler(run)]}]
    hooks['SessionEnd'] = [{'hooks': [handler(run, 3)]}]
    prefix = '^' + re.escape(f'mcp__{server_name.replace("-", "_")}__memory_')
    hooks['PreToolUse'] = [{'matcher': prefix, 'hooks': [handler(run)]}]
    return {'description': 'SWEGCA-VRS2 session ingress and exact session routing',
            'hooks': hooks}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--python', type=Path, default=Path(sys.executable))
    parser.add_argument('--state-dir', type=Path, required=True)
    parser.add_argument('--module-root', type=Path)
    parser.add_argument('--server-name', default='swegca-vrs')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    python = args.python.expanduser().absolute()
    state = args.state_dir.expanduser().resolve()
    if not python.is_file():
        parser.error('python executable unavailable')
    output = args.output.expanduser().resolve()
    if output.exists():
        parser.error(f'output already exists: {output}')
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(config(python, state, module_root=args.module_root,
                                        server_name=args.server_name),
                                 ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    output.chmod(0o600)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
