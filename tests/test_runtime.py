import builtins
import copy
import hashlib
import json
from pathlib import Path

import pytest
from pydantic import ValidationError

from swegca_vrs_mcp.runtime import Assessment, Dataset, Runtime, load_dataset

ROOT = Path(__file__).resolve().parents[1]


def dataset():
    return load_dataset(ROOT / "examples/synthetic.json")


def assessment(identifier="vrs-edge-group:7", verdict="support"):
    return Assessment(episode_id=identifier, proposition="demo-outcome", verdict=verdict,
                      rationale="Constructed conditional current observation",
                      current_evidence_refs=["fixture:current"], contradiction_refs=[])


def test_all_outcomes_hot_and_output_pagination_does_not_cap_cognition():
    runtime = Runtime(dataset())
    status = runtime.status()
    assert status["episode_count"] == 8
    assert all(status["outcome_counts"].values())
    ids, offset = [], 0
    while True:
        page = runtime.recall("demo", ["demo"], offset, 2)
        assert page["candidate_count"] == 8
        ids.extend(row["episode_id"] for row in page["candidates"])
        offset = page["next_offset"]
        if offset is None:
            break
    assert len(set(ids)) == 8
    result = runtime.evaluate("demo", ["demo"], [assessment()])
    assert [a["decision"] for a in result["arms"]] == ["success", "abstain", "abstain", "abstain", "success"]
    for arm in result["arms"]:
        assert len(arm["recalled_episode_ids"]) == 8
        assert arm["stage_order"] == ["deja_vu", "recall", "replay", "re_evidence"]
        assert not any(arm["authority"].values())
    assert not result["growth_claimed"]
    assert not result["input_authenticity_verified"]


def test_missing_current_evidence_never_inherits_historical_truth():
    result = Runtime(dataset()).evaluate("demo", ["demo"], [])
    assert all(a["decision"] == "abstain" for a in result["arms"])


def test_conflicting_current_evidence_abstains():
    data = dataset()
    data.current_vrs.strengths["vrs-edge-group:8"] = 1.2
    result = Runtime(data).evaluate("demo", ["demo"], [assessment(), assessment("vrs-edge-group:8", "refute")])
    arm = result["arms"][0]
    assert arm["decision"] == "abstain"
    assert arm["conflicting_propositions"] == ["demo-outcome"]


def test_conflicting_source_outcomes_abstain_even_when_caller_supports_both():
    data = dataset()
    data.current_vrs.strengths["vrs-edge-group:8"] = 1.2
    result = Runtime(data).evaluate("demo", ["demo"], [assessment(), assessment("vrs-edge-group:8")])
    assert result["arms"][0]["decision"] == "abstain"


def test_projection_below_threshold_revokes_promotion_not_experience():
    data = dataset()
    data.current_vrs.strengths["vrs-edge-group:7"] = 0.9
    runtime = Runtime(data)
    assert runtime.status()["episode_count"] == 8
    assert runtime.evaluate("demo", ["demo"], [assessment()])["arms"][0]["decision"] == "abstain"


def test_source_input_and_result_are_detached_and_model_agnostic():
    data = dataset()
    runtime = Runtime(data)
    before = runtime.status()
    data.episodes[0].steps[0].observation["source_item_id"] = "mutated"
    data.current_vrs.strengths["vrs-edge-group:7"] = 0.0
    output = runtime.episode("experience:success")
    output["episode"]["steps"][0]["observation"]["source_item_id"] = "mutated-output"
    assert runtime.episode("experience:success")["episode"]["steps"][0]["observation"]["source_item_id"] == "fixture:0"
    runtime.evaluate("client-A", ["demo"], [assessment()])
    runtime.evaluate("client-B", ["demo"], [assessment()])
    assert runtime.status() == before


def test_hot_requests_do_not_open_files_or_hash(monkeypatch):
    runtime = Runtime(dataset())
    def forbidden(*a, **kw):
        raise AssertionError("I/O or hash on hot core path")
    monkeypatch.setattr(builtins, "open", forbidden)
    monkeypatch.setattr(Path, "open", forbidden)
    monkeypatch.setattr(hashlib, "sha256", forbidden)
    runtime.status()
    runtime.recall("demo", ["demo"])
    runtime.episode("experience:success")
    runtime.evaluate("demo", ["demo"], [assessment()])


@pytest.mark.parametrize("value", [float("nan"), float("inf"), -1.0])
def test_invalid_strength_rejected(value):
    data = dataset().model_dump()
    data["current_vrs"]["strengths"]["vrs-edge-group:7"] = value
    with pytest.raises(ValidationError):
        Dataset.model_validate(data)


def test_duplicate_assessment_and_unknown_episode_rejected():
    runtime = Runtime(dataset())
    with pytest.raises(ValueError):
        runtime.evaluate("demo", ["demo"], [assessment(), assessment()])
    with pytest.raises(KeyError):
        runtime.evaluate("demo", ["demo"], [assessment("absent")])


def test_dataset_duplicate_json_and_unknown_fields_rejected(tmp_path):
    path = tmp_path / "duplicate.json"
    path.write_text('{"x": 1, "x": 2}', encoding="utf-8")
    with pytest.raises(ValueError):
        load_dataset(path)
    data = dataset().model_dump()
    data["authority"] = {"world": True}
    with pytest.raises(ValidationError):
        Dataset.model_validate(data)


def test_projection_content_identity_changes_even_if_claimed_report_id_does_not():
    data = dataset()
    first = Runtime(data).status()
    data.current_vrs.strengths["vrs-edge-group:7"] = 0.1
    second = Runtime(data).status()
    assert first["dataset_id"] != second["dataset_id"]
    assert first["projection_content_sha256"] != second["projection_content_sha256"]


def test_restart_same_operator_dataset_reproduces_snapshot():
    assert Runtime(dataset()).status() == Runtime(dataset()).status()


@pytest.mark.parametrize("change", ["duplicate", "unknown_strength", "unknown_repair", "same_projection"])
def test_dataset_reference_errors_rejected(change):
    data = dataset().model_dump()
    if change == "duplicate":
        data["episodes"].append(copy.deepcopy(data["episodes"][0]))
    elif change == "unknown_strength":
        data["current_vrs"]["strengths"]["absent"] = 1.1
    elif change == "unknown_repair":
        data["repair_source_ids"].append("absent")
    else:
        data["frozen_vrs"]["snapshot_id"] = data["current_vrs"]["snapshot_id"]
    with pytest.raises(ValidationError):
        Dataset.model_validate(data)
