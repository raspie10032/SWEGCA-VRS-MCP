#!/usr/bin/env python3
"""Codex MCP entry for session-first VRS with durable-main fallback."""
import json
import os
from pathlib import Path


def configure():
    codex_home = Path(os.environ.get('CODEX_HOME', Path.home() / '.codex'))
    config = Path(os.environ.setdefault('VRS2_CONFIG', str(codex_home / 'vrs2.json')))
    values = json.loads(config.read_text(encoding='utf-8')) if config.is_file() else {}
    data = Path(os.environ.get('XDG_DATA_HOME', Path.home() / '.local/share'))
    os.environ.setdefault('VRS2_STATE', str(values.get('state') or
                                             data / 'swegca-vrs2-codex'))


def main():
    configure()
    from swegca_vrs2.layered import serve
    return serve(os.environ['VRS2_STATE'])


if __name__ == '__main__':
    raise SystemExit(main())
