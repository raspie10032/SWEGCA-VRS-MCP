import copy

import pytest

from swegca_vrs_mcp.core.mosaic_paper_crossing_provenance import audit_crossing_supports


def _inputs():
    episodes = {
        name: {"episode_id": name, "revision": f"rev:{name}", "source_addresses": [f"source:{name}"], "step": {"outcome": "failure", "observation": {"source_item_id": name}}}
        for name in ("experience:a", "experience:b")
    }
    evidence = [{"episode_id": name, "revision": row["revision"], "source_addresses": row["source_addresses"], "source_item_id": name, "outcome": "failure"} for name, row in episodes.items()]
    key = {"source_term": "profile", "target_term": "outcome", "sign": -1}
    return [
        [{**key, "canonical_group_id": 7, "causal_source_experience_id": None}],
        [{**key, "edge_id": 7, "episode_ids": list(episodes), "distinct_source_episode_count": 2}],
        {"schema_version": "swegca-outcome-relation-evidence-v1", "relations": [{**key, "evidence": evidence}]},
        episodes,
    ]


def test_complete_multisource_set_is_not_unique_causal_claim():
    result = audit_crossing_supports(*_inputs())
    assert result["support_bound_group_count"] == 1
    assert result["distinct_support_episode_count"] == 2
    assert result["multi_source_support_group_count"] == 1
    assert result["runtime_eligibility_changed"] is False
    assert result["unique_causal_source_count_inferred_from_supports"] is False
    assert not any(result["authority"].values())


@pytest.mark.parametrize("field,value", [("revision", "wrong"), ("source_item_id", "wrong"), ("outcome", "success"), ("source_addresses", ["wrong"])])
def test_support_provenance_must_match_sealed_source(field, value):
    values = _inputs()
    values[2]["relations"][0]["evidence"][0][field] = value
    with pytest.raises(ValueError, match="provenance differs"):
        audit_crossing_supports(*values)


def test_wrong_canonical_group_rejected():
    values = _inputs()
    values[1][0]["edge_id"] = 8
    with pytest.raises(ValueError, match="canonical group binding"):
        audit_crossing_supports(*values)


def test_summary_cannot_drop_one_support_source():
    values = _inputs()
    values[1][0]["episode_ids"] = ["experience:a"]
    values[1][0]["distinct_source_episode_count"] = 1
    with pytest.raises(ValueError, match="summary support set"):
        audit_crossing_supports(*values)


def test_duplicate_source_rejected():
    values = _inputs()
    values[2]["relations"][0]["evidence"].append(copy.deepcopy(values[2]["relations"][0]["evidence"][0]))
    with pytest.raises(ValueError, match="missing or duplicated"):
        audit_crossing_supports(*values)


def test_unknown_is_retained_not_assigned_to_available_sources():
    values = _inputs()
    values[0].append({"source_term": "old-a", "target_term": "old-b", "sign": 1, "canonical_group_id": 9, "causal_source_experience_id": None})
    result = audit_crossing_supports(*values)
    assert result["remaining_unresolved_group_count"] == 1
    assert result["rows"][1]["support_episode_ids"] == []


def test_direct_source_requires_actual_sealed_episode():
    values = _inputs()
    values[0].append({"source_term": "experience:absent", "target_term": "old-b", "sign": 1, "canonical_group_id": 9, "causal_source_experience_id": "experience:absent"})
    with pytest.raises(ValueError, match="direct source"):
        audit_crossing_supports(*values)
