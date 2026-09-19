"""Explicit offline store maintenance; the resident owner must be stopped."""
from __future__ import annotations

import argparse
import json
from pathlib import Path

from .server import default_state_dir
from .store import Main


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--state-dir', type=Path, default=default_state_dir())
    action = parser.add_subparsers(dest='action', required=True)
    action.add_parser('status')
    action.add_parser('checkpoint')
    action.add_parser('compact')
    group = action.add_parser('consolidate')
    group.add_argument('--expected-pair', required=True)
    group.add_argument('parent_episode_ids', nargs='+')
    options = parser.parse_args()
    with_main = Main(options.state_dir, allow_maintenance=options.action != 'status')
    try:
        if options.action == 'status':
            result = with_main.status()
        elif options.action == 'checkpoint':
            result = with_main.checkpoint()
        elif options.action == 'compact':
            result = with_main.compact()
        else:
            result = with_main.consolidate(dict(
                parent_episode_ids=options.parent_episode_ids,
                expected_pair_snapshot_id=options.expected_pair))
        print(json.dumps(result, ensure_ascii=False, sort_keys=True))
    finally:
        with_main.close()


if __name__ == '__main__':
    main()
