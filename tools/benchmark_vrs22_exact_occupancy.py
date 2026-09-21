"""Exercise the production first-level exact directory near its seal threshold.

This is a structural address-index test. Repeated capsules reference one
synthetic original episode, so its address count is not an experience-count or
VRS-parameter claim. Apply host cgroup limits to the ``read`` command.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import random
import statistics
import sys
import time

from swegca_vrs2.exact_replay import ExactReplayStore
from swegca_vrs2.store import Main


PREFIX = 0xAB
SEAL_COUNT = ((1 << 16) * 7 // 10)
ADDRESS_COUNT = SEAL_COUNT + 1


def address(index: int) -> str:
    digest = hashlib.sha256(index.to_bytes(8, "little")).digest()
    return "memory:" + (bytes((PREFIX,)) + digest[:31]).hex()


def prepare(root: Path) -> dict:
    if root.exists():
        raise ValueError("benchmark_root_exists")
    root.mkdir(mode=0o700, parents=True)
    owner = Main(root / "original", allow_ingest=True)
    try:
        original = owner.ingest(dict(request_id="one",
            text="synthetic index source observation",
            source="index-test:original", revision="1"))
        episode = owner.memory.episode(original["episode_id"])
        directory = ExactReplayStore(root / "exact")
        started = time.perf_counter()
        for first in range(0, ADDRESS_COUNT, 1024):
            directory.put_many((address(index), "main", 0, episode)
                               for index in range(first, min(ADDRESS_COUNT, first + 1024)))
        key = bytes((PREFIX,)) + b"\0" * 31
        first = directory._read_segment(key, "address", 16)
        second = directory._read_segment(key, "address", 18)
        first_state = directory._level_state(first, 16)
        second_state = directory._level_state(second, 18)
        if first_state != (SEAL_COUNT, True) or second_state[0] != 1:
            raise RuntimeError("exact_level_seal_mismatch")
        result = dict(status="PREPARED", scope="synthetic exact-index high occupancy only",
            original_episode_id=original["episode_id"], addresses=ADDRESS_COUNT,
            first_level_count=first_state[0], first_level_sealed=first_state[1],
            second_level_count=second_state[0],
            ingest_seconds=time.perf_counter() - started,
            allocated_bytes=directory.allocated_bytes(),
            logical_bytes=directory.logical_bytes())
        directory.close()
        return result
    finally:
        owner.close()


def read(root: Path, samples: int) -> dict:
    directory = ExactReplayStore(root / "exact")
    try:
        opened = directory.warm_address_segments()
        prefetched = directory.warm_replay_pages(512 * 1024 ** 2)
        rng = random.Random(220922)
        selected = rng.sample(range(ADDRESS_COUNT), samples)
        elapsed = []
        for index in selected:
            identifier = address(index)
            started = time.perf_counter_ns()
            exact = directory.get(identifier)
            elapsed.append(time.perf_counter_ns() - started)
            if exact is None or exact["replay"].episode_id != identifier:
                raise RuntimeError("high_occupancy_replay_mismatch")
        elapsed.sort()
        result = dict(status="MEASURED", scope="synthetic exact-index high occupancy only",
            real_experience_scale_claimed=False, parameter_scale_claimed=False,
            samples=samples, opened_address_segments=opened,
            prefetched_allocated_bytes=prefetched,
            median_ms=statistics.median(elapsed) / 1e6,
            p99_ms=elapsed[int(samples * .99)] / 1e6,
            max_ms=elapsed[-1] / 1e6,
            at_or_above_1ms=sum(value >= 1_000_000 for value in elapsed),
            database_module_loaded="sqlite3" in sys.modules)
        control = Path("/proc/self/cgroup")
        if control.is_file():
            group = Path("/sys/fs/cgroup") / control.read_text().strip().split(":")[-1].lstrip("/")
            if (group / "memory.max").is_file():
                result["cgroup_limits"] = dict(
                    memory_max=(group / "memory.max").read_text().strip(),
                    memory_swap_max=(group / "memory.swap.max").read_text().strip(),
                    io_max=(group / "io.max").read_text().strip())
        return result
    finally:
        directory.close()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("prepare", "read"))
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--samples", type=int, default=10000)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if not 1 <= args.samples <= ADDRESS_COUNT:
        parser.error("samples must be within the prepared address count")
    result = prepare(args.root) if args.action == "prepare" else read(args.root, args.samples)
    body = json.dumps(result, ensure_ascii=False, indent=2) + "\n"
    if args.output is not None:
        if args.output.exists():
            parser.error("output already exists")
        args.output.write_text(body, encoding="utf-8")
    print(body, end="")


if __name__ == "__main__":
    main()
