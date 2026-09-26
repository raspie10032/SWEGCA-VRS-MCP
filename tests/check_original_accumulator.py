"""Compare shuffled C++ observations with the user's original Python SWEGCA.

Optional source-equivalence check; no torch install/import, wheel, network, or
old C++ dependency. The supplied original file is used only as a test oracle.
Run with: python3 tests/check_original_accumulator.py ORIGINAL_PYTHON_FILE
"""

import ast
import dataclasses
import hashlib
import json
import math
from pathlib import Path
from statistics import NormalDist
import subprocess
import sys
from typing import Literal


def main():
    original = Path(sys.argv[1]).read_text()
    names = {
        "_mark_authoritative", "_require_authoritative_state",
        "EvidenceAccumulatorConfig", "EvidenceObservation", "EvidenceGroup",
        "EvidenceAxisState", "EvidenceAccumulatorState", "AccumulatorDecision",
        "AccumulatorUpdate", "wilson_interval", "assess_accumulator", "update_accumulator",
    }
    # Select unmodified original definitions. Importing the package would load
    # torch and unrelated execution paths; the accumulator itself is stdlib.
    selected = [node for node in ast.parse(original).body if getattr(node, "name", None) in names]
    assert {node.name for node in selected} == names
    module = ast.Module(body=[ast.ImportFrom(module="__future__", names=[ast.alias(name="annotations")], level=0), *selected], type_ignores=[])
    ast.fix_missing_locations(module)
    ns = dict(math=math, hashlib=hashlib, json=json, NormalDist=NormalDist, Literal=Literal,
        dataclass=dataclasses.dataclass, asdict=dataclasses.asdict, replace=dataclasses.replace,
        _STATE_AUTHORITY=object(), _DECISION_AUTHORITY=object(), __name__=__name__)
    exec(compile(module, str(sys.argv[1]), "exec"), ns)
    config = ns["EvidenceAccumulatorConfig"]()
    output = subprocess.check_output(["build/connection-tests", "--oracle-jsonl"], text=True)
    cases = observations = 0
    for line in output.splitlines():
        if not line.startswith("ORACLE "):
            continue
        row = json.loads(line[7:])
        state = ns["EvidenceAccumulatorState"].empty(row["hypothesis"], config)
        for value in row["observations"]:
            observation = ns["EvidenceObservation"](
                row["hypothesis"], value["address"], value["source"], value["context"],
                config.required_axes[value["axis"]], ["insufficient", "support", "refute"][value["outcome"]],
                value["observed_at"], value["expires_at"], value["producer"], 0.0)
            update = ns["update_accumulator"](state, observation, config, current_step=row["current_step"])
            assert value["use"] == {"expired": 1, "insufficient": 2, "duplicate": 3, "applied": 4}[update.reason]
            state = update.state
            observations += 1
        assert row["axis_support"] == [axis.effective_support for axis in state.axes]
        assert row["axis_refute"] == [axis.effective_refute for axis in state.axes]
        assert row["axis_sources"] == [axis.source_diversity for axis in state.axes]
        assert row["source_diversity"] == min(len(state.source_families), len(state.producer_ids))
        assert row["context_diversity"] == min(len(state.context_hashes), len(state.producer_ids))
        assert row["recent_count"] == len(state.recent_outcomes)
        assert row["recent_sum"] == sum(state.recent_outcomes)
        assert row["revision"] == state.revision
        decision = ns["assess_accumulator"](state, config)
        assert row["status"] == {"accept": 1, "reject": 2, "abstain": 0}[decision.status]
        assert row["reason"] == {
            "minimum_effective_samples": 1, "source_diversity": 2,
            "axis_source_diversity": 3, "context_diversity": 4,
            "regime_change_suspected": 5, "causal_lower_bound": 6,
            "upper_bound_below_threshold": 7, "uncertain": 8,
        }[decision.reason]
        cases += 1
    assert cases >= 14
    print(f"PASS: {cases} shuffled batches, {observations} observations agree with original accumulator")
    print(f"Original SHA-256: {hashlib.sha256(original.encode()).hexdigest()}")


if __name__ == "__main__":
    main()
