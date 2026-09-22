#!/usr/bin/env python3
"""Measure exact-address Déjà vu -> Recall -> Replay on a native VRS store."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import random
import resource
import statistics
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "src"))


def percentile(values, fraction):
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int((len(ordered) - 1) * fraction))]


def rss_bytes():
    with open("/proc/self/statm", encoding="ascii") as stream:
        resident = int(stream.read().split()[1])
    return resident * os.sysconf("SC_PAGE_SIZE")


def cgroup_limits():
    """Report the limits applied to this exact process, when cgroup v2 is available."""
    try:
        line = next(row for row in Path("/proc/self/cgroup").read_text(
            encoding="ascii").splitlines() if row.startswith("0::"))
        root = Path("/sys/fs/cgroup") / line.split(":", 2)[2].lstrip("/")
        return {
            "path": line.split(":", 2)[2],
            "io_max": (root / "io.max").read_text(encoding="ascii").strip(),
            "memory_max": (root / "memory.max").read_text(encoding="ascii").strip(),
        }
    except (OSError, StopIteration, ValueError):
        return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--state-dir", type=Path, required=True)
    parser.add_argument("--iterations", type=int, default=50_000)
    parser.add_argument("--full-iterations", type=int, default=1_000,
                        help="Full four-stage calls; exact Replay lookups still use --iterations")
    parser.add_argument("--stress-iterations", type=int, default=5_000,
                        help="Repeated largest-text and largest-cue exact Replay samples")
    parser.add_argument("--seed", type=int, default=22)
    parser.add_argument("--rss-limit-gb", type=float, default=4.0)
    parser.add_argument("--ssd-limit-gbps", type=float, default=5.0)
    args = parser.parse_args()
    if not 0 < args.rss_limit_gb <= 4.0:
        parser.error("--rss-limit-gb must be within 4 GB")
    if not 0 < args.ssd_limit_gbps <= 5.0:
        parser.error("--ssd-limit-gbps must be within 5 Gbps")
    if args.iterations < 1 or args.full_iterations < 1 or args.stress_iterations < 1:
        parser.error("iteration counts must be positive")
    state = args.state_dir.expanduser().resolve()
    if list(state.rglob("*.sqlite*")):
        raise SystemExit("native benchmark state contains SQLite")

    from swegca_vrs2.store import Main
    from swegca_vrs2.resident import MAX_STORAGE_BYTES, Resident, WarmView
    from swegca_vrs2.sharded import ShardedMain

    started = time.perf_counter()
    owner = Main(state, allow_ingest=False)
    load_s = time.perf_counter() - started
    resident = Resident(owner, hot_limit=0)
    try:
        build_started = time.perf_counter()
        scanned = added = 0
        while True:
            receipt = resident.backfill_exact(512)
            scanned += receipt["scanned"]
            added += receipt["added"]
            if receipt["complete"]:
                break
            if rss_bytes() > args.rss_limit_gb * 1024 ** 3:
                raise MemoryError("RSS limit exceeded while building exact Replay directory")
        build_s = time.perf_counter() - build_started
        projection_started = time.perf_counter()
        while True:
            projection = resident.backfill_projections(1)
            if projection["complete"]:
                break
            if not projection["projections"]:
                raise ValueError("linked VRS read projection did not complete")
        projection_s = time.perf_counter() - projection_started
        ids = list(owner.memory._store["ids"][:owner.memory.episode_count])
        for shard in resident.ids():
            view = WarmView(shard, resident.bundles[shard])
            try:
                generation = view.refresh()
                ids.extend(generation.memory._store["ids"][:generation.memory.episode_count])
            finally:
                view.close()
        ids = tuple(ids)
        if not ids:
            raise ValueError("native store has no experience")
        rng = random.Random(args.seed)
        queries = [rng.choice(ids) for _ in range(args.iterations)]
        logical = ShardedMain(owner, resident)
        # Prime one key per hash prefix, then measure the exact disk Replay
        # capsule path at high sample count without repeating Re-evidence work.
        prefixes = {}
        for identifier in ids:
            prefixes.setdefault(identifier[7:9], identifier)
        cold_ms = []
        for identifier in prefixes.values():
            call = time.perf_counter_ns()
            exact = resident.exact_replay(identifier)
            cold_ms.append((time.perf_counter_ns() - call) / 1e6)
            if exact is None or exact["replay"].episode_id != identifier:
                raise ValueError("exact Replay address mismatch")
        replay_ms, replay_cpu_ms = [], []
        began = time.perf_counter()
        for identifier in queries:
            call = time.perf_counter_ns()
            call_cpu = time.thread_time_ns()
            exact = resident.exact_replay(identifier)
            replay_cpu_ms.append((time.thread_time_ns() - call_cpu) / 1e6)
            replay_ms.append((time.perf_counter_ns() - call) / 1e6)
            if exact is None or exact["replay"].episode_id != identifier:
                raise ValueError("exact Replay address mismatch")
        elapsed_s = time.perf_counter() - began
        stage_replay_ms, stage_replay_cpu_ms, total_ms = [], [], []
        for identifier in queries[:min(args.full_iterations, len(queries))]:
            call = time.perf_counter_ns()
            packet = logical.recall(identifier, None)
            total_ms.append((time.perf_counter_ns() - call) / 1e6)
            stage_replay_ms.append(packet["timings_ns"]["through_replay"] / 1e6)
            stage_replay_cpu_ms.append(packet["timings_ns"]["through_replay_thread_cpu"] / 1e6)
        stress = {}
        # ``episode_light`` deliberately omits cues, so inspect the canonical
        # compact cue column and the exact capsule's retained observation bytes.
        observation_sizes, cue_sizes = {}, {}
        for identifier in ids:
            exact = resident.exact_replay(identifier)
            replay = exact["replay"]
            observation = replay.steps[0].observation
            observation_sizes[identifier] = len(observation.payload or b"")
            cue_sizes[identifier] = exact["cue_count"]
        largest_cues = max(ids, key=cue_sizes.__getitem__)
        largest_observation = max(ids, key=observation_sizes.__getitem__)
        selected = {
            "largest_observation": largest_observation,
            "largest_cue_vector": largest_cues,
        }
        for label, identifier in selected.items():
            episode = resident.exact_replay(identifier)["replay"]
            exact_values, exact_cpu_values = [], []
            stage_values, stage_cpu_values, complete_values = [], [], []
            for _ in range(args.stress_iterations):
                call = time.perf_counter_ns()
                call_cpu = time.thread_time_ns()
                exact = resident.exact_replay(identifier)
                exact_cpu_values.append((time.thread_time_ns() - call_cpu) / 1e6)
                exact_values.append((time.perf_counter_ns() - call) / 1e6)
                if exact is None or exact["replay"].episode_id != identifier:
                    raise ValueError("stress exact Replay address mismatch")
                call = time.perf_counter_ns()
                packet = logical.recall(identifier, None)
                complete_values.append((time.perf_counter_ns() - call) / 1e6)
                stage_values.append(packet["timings_ns"]["through_replay"] / 1e6)
                stage_cpu_values.append(packet["timings_ns"]["through_replay_thread_cpu"] / 1e6)
            stress[label] = {
                "episode_id": identifier,
                "text_characters": len(str(episode.steps[0].observation.get("text", ""))),
                "observation_bytes": observation_sizes[identifier],
                "cue_count": cue_sizes[identifier],
                "samples": args.stress_iterations,
                "exact_max_ms": round(max(exact_values), 6),
                "exact_thread_cpu_max_ms": round(max(exact_cpu_values), 6),
                "through_replay_max_ms": round(max(stage_values), 6),
                "through_replay_thread_cpu_max_ms": round(max(stage_cpu_values), 6),
                "through_re_evidence_max_ms": round(max(complete_values), 6),
            }
        file_stats = [path.stat() for path in state.rglob("*") if path.is_file()]
        disk_logical = sum(item.st_size for item in file_stats)
        disk_allocated = sum((getattr(item, "st_blocks", None) or
                              ((item.st_size + 511) // 512)) * 512 for item in file_stats)
        peak = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss * 1024
        database_module_loaded = any(
            name == "sqlite3" or name.startswith("sqlite3.") for name in sys.modules)
        output = {
            "status": "PASS" if peak <= args.rss_limit_gb * 1024 ** 3
                      and disk_allocated <= MAX_STORAGE_BYTES
                      and not database_module_loaded else "FAIL",
            "acceptance_scope": "resource_limits_only; Replay timings are diagnostic",
            "state": str(state), "records": len(ids), "iterations": len(queries),
            "full_iterations": len(stage_replay_ms),
            "main_load_s": round(load_s, 6), "exact_directory_build_s": round(build_s, 6),
            "read_projection_build_s": round(projection_s, 6),
            "exact_scanned": scanned, "exact_added": added,
            "first_prefix_exact_replay_ms": {
                "prefixes": len(cold_ms), "median": round(statistics.median(cold_ms), 6),
                "p99": round(percentile(cold_ms, .99), 6), "max": round(max(cold_ms), 6),
            },
            "exact_capsule_replay_ms": {
                "median": round(statistics.median(replay_ms), 6),
                "p95": round(percentile(replay_ms, .95), 6),
                "p99": round(percentile(replay_ms, .99), 6),
                "max": round(max(replay_ms), 6),
            },
            "exact_capsule_replay_thread_cpu_ms": {
                "median": round(statistics.median(replay_cpu_ms), 6),
                "p99": round(percentile(replay_cpu_ms, .99), 6),
                "max": round(max(replay_cpu_ms), 6),
            },
            "four_stage_through_replay_ms": {
                "median": round(statistics.median(stage_replay_ms), 6),
                "p95": round(percentile(stage_replay_ms, .95), 6),
                "p99": round(percentile(stage_replay_ms, .99), 6),
                "max": round(max(stage_replay_ms), 6),
            },
            "four_stage_through_replay_thread_cpu_ms": {
                "median": round(statistics.median(stage_replay_cpu_ms), 6),
                "p99": round(percentile(stage_replay_cpu_ms, .99), 6),
                "max": round(max(stage_replay_cpu_ms), 6),
            },
            "through_re_evidence_wall_ms": {
                "median": round(statistics.median(total_ms), 6),
                "p95": round(percentile(total_ms, .95), 6),
                "p99": round(percentile(total_ms, .99), 6),
                "max": round(max(total_ms), 6),
            },
            "largest_record_stress": stress,
            "lookup_elapsed_s": round(elapsed_s, 6),
            "lookups_per_s": round(len(queries) / elapsed_s, 1),
            "rss_bytes": rss_bytes(), "peak_rss_bytes": peak,
            "rss_limit_bytes": int(args.rss_limit_gb * 1024 ** 3),
            "disk_allocated_bytes": disk_allocated,
            "disk_logical_bytes": disk_logical,
            "storage_limit_bytes": MAX_STORAGE_BYTES,
            "database_module_loaded": database_module_loaded,
            "ssd_limit_bps": int(args.ssd_limit_gbps * 1_000_000_000 / 8),
            "cgroup_limits": cgroup_limits(),
            "ssd_limit_note": "cgroup values are evidence only when io_max names this state device",
        }
        print(json.dumps(output, ensure_ascii=False, indent=2))
        return 0 if output["status"] == "PASS" else 2
    finally:
        resident.close()
        owner.close()


if __name__ == "__main__":
    raise SystemExit(main())
