"""Measure natural Recall through original Replay on a copy of native experience.

The temporary ReplayResult wrapper records a stage boundary and returns the
unchanged product result. The measured read still uses ShardedMain, complete
VRS region/navigation logic, original capsules and Re-evidence. Pass a copied,
fully prepared native state; this tool does not use the resident user state.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib
import json
from pathlib import Path
import re
import statistics
import sys
import time

from swegca_vrs2.resident import Resident
from swegca_vrs2.sharded import ShardedMain
from swegca_vrs2.store import Main


def percentile(values: list[int], fraction: float) -> float:
    return sorted(values)[min(len(values) - 1, int(len(values) * fraction))] / 1e6


def run(root: Path, *, sparse: int, medium: int, broad: int) -> dict:
    if not (root / 'vrs-store.json').is_file():
        raise ValueError('native_store_required')
    owner = Main(root, allow_ingest=False)
    resident = None
    projection = importlib.import_module('swegca_vrs2.projected_recall')
    original_replay_result = projection.ReplayResult
    original_replayed_episode = projection.ReplayedEpisode
    clock: dict[str, int | None | str] = {
        'began': None, 'first_replay': None, 'first_episode_id': None, 'replay': None}

    def stage_first_episode(*args, **kwargs):
        result = original_replayed_episode(*args, **kwargs)
        if clock['first_replay'] is None:
            clock['first_replay'] = time.perf_counter_ns()
            clock['first_episode_id'] = result.episode_id
        return result

    def stage_replay(*args, **kwargs):
        result = original_replay_result(*args, **kwargs)
        clock['replay'] = time.perf_counter_ns()
        return result

    projection.ReplayedEpisode = stage_first_episode
    projection.ReplayResult = stage_replay
    try:
        resident = Resident(owner, hot_limit=1)
        if not resident.read_directory_complete():
            raise ValueError('exact_read_index_not_prepared')
        main = ShardedMain(owner, resident)
        vocab = owner.memory._store['vocab']
        choices = []
        for cue_id, postings in owner.memory._store['postings'].items():
            cue = vocab.string_of(cue_id)
            if re.fullmatch(r'[a-z]{5,30}', cue) and len(postings) > 0:
                choices.append((len(postings), cue))
        if not choices:
            raise ValueError('no_eligible_natural_cues')

        cases = []
        for desired, repetitions in ((1, sparse), (100, medium), (1000, broad)):
            fanout, cue = min(choices, key=lambda row: (abs(row[0] - desired), row[0]))
            # The first call is reported separately; it is not silently removed.
            first_replay, through_replay, full = [], [], []
            candidates = None
            for _ in range(repetitions):
                clock['began'] = time.perf_counter_ns()
                clock['first_replay'] = None
                clock['first_episode_id'] = None
                clock['replay'] = None
                result = main.recall(cue, None)
                finished = time.perf_counter_ns()
                if clock['first_replay'] is None or clock['replay'] is None:
                    raise RuntimeError('replay_stage_not_reached')
                activation = result['receipt']['activation']
                if (len(activation.recall.candidates) != len(activation.replay.episodes)
                        or len(activation.replay.episodes) != fanout
                        or activation.re_evidence is None):
                    raise RuntimeError('four_stage_candidate_mismatch')
                if clock['first_episode_id'] != activation.recall.candidates[0].episode_id:
                    raise RuntimeError('first_replay_does_not_match_first_ranked_candidate')
                candidates = len(activation.replay.episodes)
                first_replay.append(clock['first_replay'] - clock['began'])
                through_replay.append(clock['replay'] - clock['began'])
                full.append(finished - clock['began'])
            warm = through_replay[1:] or through_replay
            first_warm = first_replay[1:] or first_replay
            cases.append(dict(target_fanout=desired, actual_fanout=fanout,
                cue_sha256=hashlib.sha256(cue.encode('utf-8')).hexdigest(),
                candidate_count=candidates, calls=repetitions,
                first_ranked_original_replay_ms=first_replay[0] / 1e6,
                warm_median_first_ranked_original_replay_ms=statistics.median(first_warm) / 1e6,
                first_ranked_original_replay_at_or_above_1ms=sum(
                    value >= 1_000_000 for value in first_replay),
                first_through_replay_ms=through_replay[0] / 1e6,
                warm_median_through_replay_ms=statistics.median(warm) / 1e6,
                warm_p99_through_replay_ms=percentile(warm, .99),
                warm_max_through_replay_ms=max(warm) / 1e6,
                through_replay_at_or_above_1ms=sum(value >= 1_000_000
                                                    for value in through_replay),
                first_full_four_stage_ms=full[0] / 1e6,
                full_four_stage_median_ms=statistics.median(full) / 1e6))
        control = Path('/proc/self/cgroup')
        limits = {}
        if control.is_file():
            group = Path('/sys/fs/cgroup') / control.read_text().strip().split(':')[-1].lstrip('/')
            for name in ('memory.max', 'memory.swap.max', 'io.max', 'memory.peak'):
                path = group / name
                if path.is_file():
                    limits[name] = path.read_text().strip()
        return dict(status='MEASURED', scope='copied existing native experience, natural '
                    'ShardedMain Recall through Replay',
                    record_count=owner.memory.episode_count, cases=cases,
                    cgroup_limits=limits, sqlite_module_loaded='sqlite3' in sys.modules)
    finally:
        projection.ReplayedEpisode = original_replayed_episode
        projection.ReplayResult = original_replay_result
        if resident is not None:
            resident.close()
        owner.close()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--sparse', type=int, default=100)
    parser.add_argument('--medium', type=int, default=10)
    parser.add_argument('--broad', type=int, default=3)
    args = parser.parse_args()
    if min(args.sparse, args.medium, args.broad) < 2:
        parser.error('each case needs at least two calls to separate first from warm reads')
    result = run(args.root.resolve(), sparse=args.sparse,
                 medium=args.medium, broad=args.broad)
    body = json.dumps(result, ensure_ascii=False, indent=2) + '\n'
    if args.output:
        if args.output.exists():
            parser.error('output already exists')
        args.output.write_text(body, encoding='utf-8')
    print(body, end='')


if __name__ == '__main__':
    main()
