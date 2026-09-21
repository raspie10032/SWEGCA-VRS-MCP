"""Locate first natural-Replay latency on a prepared native experience copy.

The probe wraps the production cue-table slot lookup and ReplayResult
constructor without changing their returned values. No resident user state is
opened. Use a separate process per run, under the resource-limited cgroup.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib
import json
import os
from pathlib import Path
import re
import resource
import time

from swegca_vrs2.cue_shards import CueShardDirectory
from swegca_vrs2.resident import Resident
from swegca_vrs2.sharded import ShardedMain
from swegca_vrs2.store import Main


def profile(root: Path, *, prewarm_table: bool) -> dict:
    table = root / 'exact-replay' / 'cue-shards' / 'table-p12.vrs'
    if not table.is_file():
        raise ValueError('prepared_cue_directory_required')
    with table.open('rb') as stream:
        os.posix_fadvise(stream.fileno(), 0, 0, os.POSIX_FADV_DONTNEED)
    warm_seconds = None
    if prewarm_table:
        began = time.perf_counter_ns()
        with table.open('rb') as stream:
            while stream.read(1024 * 1024):
                pass
        warm_seconds = (time.perf_counter_ns() - began) / 1e9

    owner = Main(root, allow_ingest=False)
    resident = None
    module = importlib.import_module('swegca_vrs2.projected_recall')
    original_probe = CueShardDirectory._probe
    original_replay = module.ReplayResult
    timings: dict[str, int | None] = {'began': None, 'replay': None,
                                      'probe_ns': 0, 'probe_calls': 0}

    def timed_probe(self, *args, **kwargs):
        began = time.perf_counter_ns()
        try:
            return original_probe(self, *args, **kwargs)
        finally:
            timings['probe_ns'] += time.perf_counter_ns() - began
            timings['probe_calls'] += 1

    def timed_replay(*args, **kwargs):
        result = original_replay(*args, **kwargs)
        timings['replay'] = time.perf_counter_ns()
        return result

    CueShardDirectory._probe = timed_probe
    module.ReplayResult = timed_replay
    try:
        resident = Resident(owner, hot_limit=1)
        if not resident.read_directory_complete():
            raise ValueError('read_directory_not_complete')
        vocab = owner.memory._store['vocab']
        choices = [(len(rows), vocab.string_of(cue_id))
                   for cue_id, rows in owner.memory._store['postings'].items()
                   if re.fullmatch(r'[a-z]{5,30}', vocab.string_of(cue_id))]
        fanout, cue = min(choices, key=lambda row: (abs(row[0] - 1), row[0]))
        usage_before = resource.getrusage(resource.RUSAGE_SELF)
        timings['began'] = time.perf_counter_ns()
        result = ShardedMain(owner, resident).recall(cue, None)
        finished = time.perf_counter_ns()
        usage_after = resource.getrusage(resource.RUSAGE_SELF)
        replay = result['receipt']['activation'].replay
        if timings['replay'] is None or len(replay.episodes) != fanout:
            raise RuntimeError('first_replay_incomplete')
        return dict(status='MEASURED', scope='one first natural Replay on copied native main',
            record_count=owner.memory.episode_count,
            cue_sha256=hashlib.sha256(cue.encode('utf-8')).hexdigest(),
            matched_experiences=fanout, prewarm_table=prewarm_table,
            prewarm_seconds=warm_seconds, table_bytes=table.stat().st_size,
            through_replay_ms=(timings['replay'] - timings['began']) / 1e6,
            full_four_stage_ms=(finished - timings['began']) / 1e6,
            cue_probe_ms=timings['probe_ns'] / 1e6,
            cue_probe_calls=timings['probe_calls'],
            minor_faults=usage_after.ru_minflt - usage_before.ru_minflt,
            major_faults=usage_after.ru_majflt - usage_before.ru_majflt)
    finally:
        CueShardDirectory._probe = original_probe
        module.ReplayResult = original_replay
        if resident is not None:
            resident.close()
        owner.close()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--prewarm-table', action='store_true')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    result = profile(args.root.resolve(), prewarm_table=args.prewarm_table)
    body = json.dumps(result, ensure_ascii=False, indent=2) + '\n'
    if args.output:
        if args.output.exists():
            parser.error('output already exists')
        args.output.write_text(body, encoding='utf-8')
    print(body, end='')


if __name__ == '__main__':
    main()
