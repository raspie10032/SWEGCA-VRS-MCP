"""Read-only support-lineage diagnostic; never grants causal eligibility."""

from __future__ import annotations

from collections.abc import Mapping, Sequence
from typing import Any


def _key(row: Mapping[str, Any]) -> tuple[str, str, int]:
    source, target, sign = row["source_term"], row["target_term"], row["sign"]
    if not isinstance(source, str) or not source or not isinstance(target, str) or not target or sign not in (-1, 1):
        raise ValueError("relation identity changed")
    return source, target, sign


def audit_crossing_supports(
    crossings: Sequence[Mapping[str, Any]],
    relation_summary: Sequence[Mapping[str, Any]],
    relation_evidence: Mapping[str, Any],
    episodes: Mapping[str, Mapping[str, Any]],
) -> dict[str, Any]:
    """Bind endpoint/sign/group and complete support sets to sealed episodes."""
    if relation_evidence.get("schema_version") != "swegca-outcome-relation-evidence-v1":
        raise ValueError("relation evidence schema changed")
    evidence = {}
    for row in relation_evidence["relations"]:
        key = _key(row)
        if key in evidence or not row["evidence"]:
            raise ValueError("duplicate or empty relation evidence")
        evidence[key] = row["evidence"]
    summary = {}
    for row in relation_summary:
        key = _key(row)
        if key in summary:
            raise ValueError("duplicate relation summary")
        summary[key] = row
    rows = []
    group_ids = set()
    union = set()
    for crossing in crossings:
        key = _key(crossing)
        group_id = int(crossing["canonical_group_id"])
        if group_id in group_ids:
            raise ValueError("duplicate crossing group")
        group_ids.add(group_id)
        direct = crossing.get("causal_source_experience_id")
        supports = set()
        if direct is not None:
            candidates = {term for term in key[:2] if term.startswith("experience:")}
            if candidates != {direct} or direct not in episodes:
                raise ValueError("direct source is not bound to sealed episode")
            supports.add(direct)
        matched = evidence.get(key)
        if matched is not None:
            declared = summary.get(key)
            if declared is None or int(declared["edge_id"]) != group_id:
                raise ValueError("canonical group binding changed")
            seen = set()
            source_items = set()
            for support in matched:
                identity = support["episode_id"]
                episode = episodes.get(identity)
                if episode is None or identity in seen or support["source_item_id"] in source_items:
                    raise ValueError("support source missing or duplicated")
                observation = episode["step"]["observation"]
                if (
                    episode["episode_id"] != identity
                    or support["revision"] != episode["revision"]
                    or not support["revision"]
                    or support["source_addresses"] != episode["source_addresses"]
                    or not support["source_addresses"]
                    or support["outcome"] != episode["step"]["outcome"]
                    or support["source_item_id"] != observation["source_item_id"]
                ):
                    raise ValueError("support provenance differs from sealed episode")
                seen.add(identity)
                source_items.add(support["source_item_id"])
            if set(declared["episode_ids"]) != seen or int(declared["distinct_source_episode_count"]) != len(seen):
                raise ValueError("summary support set differs from evidence")
            supports.update(seen)
        elif key in summary:
            raise ValueError("summary has no supporting evidence payload")
        union.update(supports)
        rows.append({
            "canonical_group_id": group_id,
            "source_term": key[0],
            "target_term": key[1],
            "sign": key[2],
            "original_endpoint_source": direct,
            "support_episode_ids": sorted(supports),
            "support_source_count": len(supports),
            "status": "sealed_support_set_bound" if supports else "unresolved_in_available_support_artifacts",
            "unique_source_attribution_claimed": False,
            "causal_effect_claimed": False,
        })
    return {
        "schema_version": "swegca-paper-crossing-support-provenance-audit-v1",
        "crossing_group_count": len(rows),
        "original_endpoint_unresolved_count": sum(row["original_endpoint_source"] is None for row in rows),
        "support_bound_group_count": sum(bool(row["support_episode_ids"]) for row in rows),
        "remaining_unresolved_group_count": sum(not row["support_episode_ids"] for row in rows),
        "unique_source_support_group_count": sum(row["support_source_count"] == 1 for row in rows),
        "multi_source_support_group_count": sum(row["support_source_count"] > 1 for row in rows),
        "distinct_support_episode_count": len(union),
        "support_episode_ids": sorted(union),
        "rows": rows,
        "runtime_eligibility_changed": False,
        "unique_causal_source_count_inferred_from_supports": False,
        "new_experience_count": 0,
        "growth_claimed": False,
        "authority": {key: False for key in ("semantic", "world", "action", "persistent_write", "model_update", "distribution", "p3")},
    }
