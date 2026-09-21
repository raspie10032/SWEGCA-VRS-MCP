#!/usr/bin/env python3
"""Compare one and sixteen VRS region workers on an existing native shard."""
from __future__ import annotations

import argparse
import gc
import json
import os
from pathlib import Path
import resource
import stat
import subprocess
import sys
import threading
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "src"))


def cgroup_limits():
    line = next(row for row in Path("/proc/self/cgroup").read_text().splitlines()
                if row.startswith("0::"))
    path = Path("/sys/fs/cgroup") / line.split(":", 2)[2].lstrip("/")
    return {"path": line.split(":", 2)[2],
            "memory_max": (path / "memory.max").read_text().strip(),
            "memory_swap_max": (path / "memory.swap.max").read_text().strip(),
            "io_max": (path / "io.max").read_text().strip(),
            "memory_peak": (path / "memory.peak").read_text().strip()}


def measure(prepared, workers, cycles):
    from swegca_vrs2.store import Main
    stop = threading.Event()
    peak = [0]

    def sample_threads():
        while not stop.wait(.0005):
            peak[0] = max(peak[0], len(os.listdir("/proc/self/task")))

    sampler = threading.Thread(target=sample_threads, daemon=True)
    sampler.start()
    wall = time.perf_counter()
    cpu = time.process_time()
    try:
        graph = Main.consolidate_run(prepared, cycles=cycles, workers=workers)
    finally:
        stop.set()
        sampler.join()
    elapsed = time.perf_counter() - wall
    cpu_elapsed = time.process_time() - cpu
    result = {"workers_requested": workers, "cycles": cycles,
              "wall_seconds": round(elapsed, 6),
              "process_cpu_seconds": round(cpu_elapsed, 6),
              "peak_process_threads": peak[0],
              "version_id": graph.stable.version_id,
              "fine_regions_seconds": graph.stable.fine_seconds,
              "version_seconds": graph.stable.seconds,
              "regions": len(set(graph.stable.labels.tolist())),
              "nodes": graph.flat.count, "edges": len(graph.flat.src)}
    del graph
    gc.collect()
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--state-dir", type=Path, required=True)
    parser.add_argument("--io-device", type=Path, required=True,
                        help="Physical block disk backing the state filesystem")
    parser.add_argument("--cycles", type=int, default=4)
    args = parser.parse_args()
    if args.cycles < 1:
        parser.error("cycles must be positive")
    state = args.state_dir.resolve()
    if not state.is_dir():
        parser.error("native state missing")
    io_device = args.io_device.resolve()
    if not stat.S_ISBLK(io_device.stat().st_mode):
        parser.error("I/O device is not a block disk")
    source = subprocess.check_output(["findmnt", "-T", str(state), "-no", "SOURCE"],
                                     text=True).strip().split("[", 1)[0]
    parent = subprocess.check_output(["lsblk", "-no", "PKNAME", source],
                                     text=True).strip()
    if io_device.name != (parent or Path(source).name):
        parser.error("I/O device does not back the state filesystem")
    from swegca_vrs2.native_journal import is_native_store
    from swegca_vrs2.store import Main
    from swegca_vrs2.resident import MAX_STORAGE_BYTES
    from swegca_vrs2 import vrs_refine
    if not is_native_store(state):
        parser.error("state is not native VRS")
    if vrs_refine.WORKERS < 16:
        parser.error("runtime does not enable sixteen workers")
    files = [path for path in state.rglob("*") if path.is_file()]
    if any("sqlite" in path.name.lower() or path.suffix.lower() == ".db"
           for path in files):
        parser.error("legacy database found")
    disk_allocated = sum((item.stat().st_blocks or
                          (item.stat().st_size + 511) // 512) * 512 for item in files)
    device = f"{os.major(io_device.stat().st_rdev)}:{os.minor(io_device.stat().st_rdev)}"
    owner = Main(state, allow_ingest=False)
    try:
        prepared = owner.consolidate_prepare()
        serial = measure(prepared, 1, args.cycles)
        parallel = measure(prepared, 16, args.cycles)
        limits = cgroup_limits()
        actual_memory = int(limits["memory_peak"])
        actual_io = limits["io_max"]
        output = {"status": "PASS" if serial["version_id"] == parallel["version_id"]
                  and parallel["peak_process_threads"] - serial["peak_process_threads"] >= 16
                  and actual_memory <= 4 * 1024 ** 3
                  and limits["memory_max"] == str(4 * 1024 ** 3)
                  and limits["memory_swap_max"] == "0"
                  and disk_allocated <= MAX_STORAGE_BYTES
                  and actual_io.startswith(device + " ")
                  and "rbps=625000000" in actual_io
                  and "wbps=625000000" in actual_io else "FAIL",
                  "state": str(state),
                  "records": owner.memory.episode_count,
                  "serial": serial, "parallel": parallel,
                  "wall_speedup": round(serial["wall_seconds"] / parallel["wall_seconds"], 4),
                  "openblas_threads": os.environ.get("OPENBLAS_NUM_THREADS"),
                  "io_device": str(io_device), "state_mount_source": source,
                  "disk_allocated_bytes": disk_allocated,
                  "storage_limit_bytes": MAX_STORAGE_BYTES,
                  "cgroup": limits,
                  "database_module_loaded": any(name == "sqlite3" or name.startswith("sqlite3.")
                                                for name in sys.modules),
                  "peak_rss_bytes": resource.getrusage(resource.RUSAGE_SELF).ru_maxrss * 1024}
        if output["database_module_loaded"]:
            output["status"] = "FAIL"
        print(json.dumps(output, indent=2))
        return 0 if output["status"] == "PASS" else 1
    finally:
        owner.close()


if __name__ == "__main__":
    raise SystemExit(main())
