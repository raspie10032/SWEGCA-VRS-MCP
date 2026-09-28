#!/usr/bin/env python3
"""Feed original file paths directly to native codec/core/block ingestion.
No staging, tar, hex payload, pre-ingestion copy, or second codec pass.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('root', type=Path)
    parser.add_argument('inputs', type=Path, nargs='+')
    parser.add_argument('--block-originals', type=int, default=1024)
    parser.add_argument('--gpus', type=int, default=2, choices=(0, 1, 2))
    args = parser.parse_args()
    root = args.root.resolve()
    if root.exists() and any(root.iterdir()) and not (root / 'layout.block').is_file():
        parser.error('unrecognized output root; existing files are preserved')
    binary = Path(__file__).resolve().parents[1] / 'build/whole-file-ingress-gpu'
    process = subprocess.Popen([
        'taskset', '-c', '3-7,11-15', str(binary), str(root),
        '--gpus', str(args.gpus), '--block-originals', str(args.block_originals),
    ], stdin=subprocess.PIPE, text=True)
    visited = set()
    ticket = 0
    try:
        for item in args.inputs:
            if item.is_file():
                batches = [(item.parent, [], [item.name])]
            else:
                batches = os.walk(item, followlinks=False)
            for parent, dirs, files in batches:
                current = Path(parent).resolve()
                if current == root or root in current.parents:
                    dirs[:] = []
                    continue
                stat = current.stat()
                key = (stat.st_dev, stat.st_ino)
                # A direct file input must not suppress siblings in a later root.
                if not item.is_file():
                    if key in visited:
                        dirs[:] = []
                        continue
                    visited.add(key)
                for name in files:
                    path = Path(parent) / name
                    if path.is_symlink() or not path.is_file():
                        continue
                    ticket += 1
                    request = dict(path=str(path.resolve()), source=str(path.resolve()),
                                   media='application/octet-stream', ticket=ticket)
                    process.stdin.write(json.dumps(request, ensure_ascii=False) + '\n')
        process.stdin.close()
    except BaseException:
        # A feeder failure is not a successful session end. Leave live blocks
        # resumable instead of sending a normal EOF that would seal them.
        process.terminate()
        try:
            process.stdin.close()
        except BrokenPipeError:
            pass
        process.wait()
        raise
    return process.wait()


if __name__ == '__main__':
    raise SystemExit(main())
