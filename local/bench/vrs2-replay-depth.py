#!/usr/bin/env python3
"""Measure the maximum five-level exact Replay route without claiming scale."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
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
    args = parser.parse_args()
    state = args.state_dir.expanduser().resolve()
    if state.exists() and any(state.iterdir()):
        parser.error("--state-dir must be absent or empty")
    if args.iterations < 1:
        parser.error("--iterations must be positive")
    state.mkdir(mode=0o700, parents=True, exist_ok=True)

    from swegca_vrs2.exact_replay import HEADER_BYTES, ExactReplayStore, _key
    from swegca_vrs2.resident import MAX_RSS_BYTES, MAX_STORAGE_BYTES
    from swegca_vrs2.store import Main

    main = Main(state / "main", allow_ingest=True)
    try:
        stored = main.ingest({
            "request_id": "maximum-level-route",
            "text": "maximum exact Replay level route",
            "source": "benchmark:maximum-level-route",
            "revision": "1",
            "outcome": "pending",
        })
        identifier = stored["episode_id"]
        episode = main.memory.episode(identifier)
        key = _key(identifier)
        exact = ExactReplayStore(state / "exact")
        levels = exact.slot_powers
        try:
            for slot_power in levels[:-1]:
                fd, mapping = exact._open_segment(
                    key, create=True, namespace="address", slot_power=slot_power)
                try:
                    exact._write_level_state(mapping, 0, True)
                    mapping.flush(0, HEADER_BYTES)
                    os.fsync(fd)
                finally:
                    mapping.close()
                    os.close(fd)
            if not exact.put(identifier, "main", 0, episode):
                raise ValueError("maximum_level_route_not_published")
        finally:
            exact.close()
    finally:
        main.close()

    exact = ExactReplayStore(state / "exact")
    try:
        opened = exact.warm_address_segments()
        first_started = time.perf_counter_ns()
        first = exact.get(identifier)
        first_ms = (time.perf_counter_ns() - first_started) / 1e6
        if first is None or first["replay"].episode_id != identifier:
            raise ValueError("maximum_level_route_address_mismatch")
        values = []
        for _ in range(args.iterations):
            started = time.perf_counter_ns()
            found = exact.get(identifier)
            values.append((time.perf_counter_ns() - started) / 1e6)
            if found is None or found["replay"].episode_id != identifier:
                raise ValueError("maximum_level_route_address_mismatch")
        files = [path.stat() for path in state.rglob("*") if path.is_file()]
        allocated = sum(item.st_blocks * 512 for item in files)
        logical = sum(item.st_size for item in files)
        peak = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss * 1024
        database_module_loaded = any(
            name == "sqlite3" or name.startswith("sqlite3.") for name in sys.modules)
        output = {
            "status": "PASS" if max(values) < 1.0 and first_ms < 1.0
                      and peak <= MAX_RSS_BYTES and allocated <= MAX_STORAGE_BYTES
                      and not database_module_loaded else "FAIL",
            "scope": "synthetic maximum directory route only",
            "experience_scale_claimed": False,
            "parameter_scale_claimed": False,
            "synthetic_sealed_empty_levels": list(levels[:-1]),
            "target_level": levels[-1],
            "address_levels": list(levels),
            "opened_address_segments": opened,
            "iterations": len(values),
            "first_ms": round(first_ms, 6),
            "median_ms": round(statistics.median(values), 6),
            "p99_ms": round(percentile(values, .99), 6),
            "max_ms": round(max(values), 6),
            "at_or_above_1ms": sum(value >= 1.0 for value in values),
            "rss_bytes": rss_bytes(),
            "peak_rss_bytes": peak,
            "rss_limit_bytes": MAX_RSS_BYTES,
            "allocated_bytes": allocated,
            "logical_bytes": logical,
            "storage_limit_bytes": MAX_STORAGE_BYTES,
            "database_module_loaded": database_module_loaded,
            "cgroup_limits": cgroup_limits(),
        }
        print(json.dumps(output, ensure_ascii=False, indent=2))
        return 0 if output["status"] == "PASS" else 2
    finally:
        exact.close()


if __name__ == "__main__":
    raise SystemExit(main())
