#!/usr/bin/env python3
"""Replay the v018 checkpoint edits in a data-only, storage-independent fixture."""
from __future__ import annotations

import argparse
from contextlib import contextmanager
from hashlib import sha256
from io import BytesIO
import importlib.util
import json
from pathlib import Path
import sys
import types
from unittest.mock import patch
import warnings
import zipfile

import numpy as np


def archive_body(*, duplicate: str | None = None, extra: bool = False) -> bytes:
    metadata = {
        "format": 1,
        "identity": "clean-fixture",
        "seq": 1,
        "pair": "vrs-snapshot",
        "memory_snapshot": "memory-snapshot",
        "vrs_snapshot": "vrs-snapshot",
        "numeric": {
            "direct": "event_direct",
            "score": "event_score",
            "strength": "event_strength",
            "unresolved": "event_unresolved",
            "edges": "event_edges",
        },
        "episodes": [],
        "outcomes": {},
        "superseded": {},
        "nodes": {},
        "components": [],
        "regions": [],
        "operations": {},
        "receipt": {},
    }
    arrays = {
        "event_direct.npy": np.asarray([], dtype=np.float64),
        "event_score.npy": np.asarray([], dtype=np.float64),
        "event_strength.npy": np.asarray([], dtype=np.float64),
        "event_unresolved.npy": np.asarray([], dtype=np.bool_),
        "event_edges.npy": np.asarray([], dtype=np.int64),
    }
    output = BytesIO()
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        archive.writestr("metadata.json", json.dumps(metadata))
        encoded = {}
        for name, values in arrays.items():
            body = BytesIO()
            np.save(body, values, allow_pickle=False)
            encoded[name] = body.getvalue()
            archive.writestr(name, encoded[name])
        if extra:
            archive.writestr("unrelated.txt", "allowed")
        if duplicate == "metadata":
            with warnings.catch_warnings():
                warnings.simplefilter("ignore", UserWarning)
                archive.writestr("metadata.json", json.dumps(metadata))
        elif duplicate == "array":
            with warnings.catch_warnings():
                warnings.simplefilter("ignore", UserWarning)
                archive.writestr("event_direct.npy", encoded["event_direct.npy"])
    return output.getvalue()


@contextmanager
def loaded_checkpoint(path: Path, label: str):
    package_name = "clean_checkpoint_" + label.replace("-", "_")
    package = types.ModuleType(package_name)
    package.__path__ = []
    store = types.ModuleType(package_name + ".store")

    class HotIndex:
        def __init__(self, snapshot_id, records, postings, outcomes, propositions, superseded):
            self.snapshot_id = snapshot_id
            self.records = records
            self.outcome_counts = outcomes
            self.episode_count = len(records)

    class EventVrsInputs:
        def __init__(self, snapshot_id, **values):
            self.snapshot_id = snapshot_id
            self.__dict__.update(values)

    class EndpointDependencyIndex:
        @staticmethod
        def build(_edges):
            return object()

    class FullCurrentMemoryVrsSnapshot:
        def __init__(self, _memory, snapshot_id):
            self.snapshot_id = snapshot_id

    class Graph:
        def __init__(self, *values):
            self.values = values

    for name, value in {
        "HotIndex": HotIndex,
        "Graph": Graph,
        "MemoryEpisode": type("MemoryEpisode", (), {}),
        "MemoryStep": type("MemoryStep", (), {}),
        "FullCurrentMemoryVrsSnapshot": FullCurrentMemoryVrsSnapshot,
        "EventVrsInputs": EventVrsInputs,
        "EndpointDependencyIndex": EndpointDependencyIndex,
        "ConnectivityRegions": type("ConnectivityRegions", (), {}),
        "frozen": lambda value: value,
        "retrieval_keys": lambda _text: (),
        "freeze_view": lambda value: value,
    }.items():
        setattr(store, name, value)
    engine = types.ModuleType(package_name + ".engine")
    engine.__path__ = []
    region_arrays = types.ModuleType(package_name + ".engine.mosaic_vrs_region_arrays")
    region_arrays.RegionTermArrays = type("RegionTermArrays", (), {})
    modules = {
        package_name: package,
        package_name + ".store": store,
        package_name + ".engine": engine,
        package_name + ".engine.mosaic_vrs_region_arrays": region_arrays,
    }
    previous = {name: sys.modules.get(name) for name in modules}
    sys.modules.update(modules)
    module_name = package_name + ".checkpoint"
    spec = importlib.util.spec_from_file_location(module_name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[module_name] = module
    try:
        assert spec.loader is not None
        spec.loader.exec_module(module)
        yield module
    finally:
        sys.modules.pop(module_name, None)
        for name, value in previous.items():
            if value is None:
                sys.modules.pop(name, None)
            else:
                sys.modules[name] = value


def rejection_before_read(module, kind: str) -> bool:
    body = archive_body(duplicate=kind)
    with patch.object(zipfile.ZipFile, "read",
                      side_effect=AssertionError("member read before duplicate rejection")):
        try:
            module.decode(body, identity="clean-fixture", seq=1, pair="vrs-snapshot")
        except ValueError as error:
            return str(error) == "checkpoint_integrity_failed"
        except AssertionError:
            return False
    return False


def validate(path: Path, label: str) -> dict:
    source = path.read_bytes()
    with loaded_checkpoint(path, label) as module:
        valid_extra = module.decode(
            archive_body(extra=True),
            identity="clean-fixture",
            seq=1,
            pair="vrs-snapshot",
        )[2].snapshot_id == "vrs-snapshot"
        metadata = rejection_before_read(module, "metadata")
        array = rejection_before_read(module, "array")
    return {
        "path": str(path),
        "sha256": sha256(source).hexdigest(),
        "valid_archive_with_unrelated_extra": valid_extra,
        "duplicate_metadata_rejected_before_read": metadata,
        "duplicate_array_rejected_before_read": array,
        "pass": bool(valid_extra and metadata and array),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--candidate", action="append", nargs=2,
                        metavar=("LABEL", "PATH"), required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    baseline = validate(args.baseline.resolve(), "baseline")
    candidates = {
        label: validate(Path(path).resolve(), label)
        for label, path in args.candidate
    }
    result = {
        "schema": "vrs22-clean-checkpoint-output-validation-v1",
        "fixture": "in-memory data-only checkpoint boundary",
        "baseline_detected": baseline["pass"] is False,
        "baseline": baseline,
        "candidates": candidates,
        "all_candidates_pass": all(row["pass"] for row in candidates.values()),
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n",
                           encoding="utf-8")
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0 if result["baseline_detected"] and result["all_candidates_pass"] else 2


if __name__ == "__main__":
    raise SystemExit(main())
