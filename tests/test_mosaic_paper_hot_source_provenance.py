from __future__ import annotations

import builtins
import hashlib
import json
from dataclasses import replace

import pytest

from swegca_vrs_mcp.core.mosaic_memory_activation import (
    CurrentEvidenceVerdict,
    FullCurrentMemoryVrsSnapshot,
    MemoryEpisode,
    MemoryStep,
    activate_memory,
    build_memory_activation_index,
)
from swegca_vrs_mcp.core.mosaic_paper_hot_causal_ablation import (
    EpisodeRoleIndex,
    HotCausalAblationEngine,
    VrsPromotionProjection,
)
from swegca_vrs_mcp.core.mosaic_paper_hot_source_provenance import (
    HotSourceProvenance,
    ProvenanceEpisodeRoles,
)

CURRENT = "a" * 64
FROZEN = "b" * 64


def _episode(name, cue, outcome, observation, revision="r1"):
    return MemoryEpisode(
        episode_id=name,
        cues=(cue,),
        steps=(
            MemoryStep(
                "fixture",
                observation,
                ("fixture",),
                "historical",
                outcome,
                (f"fixture:{name}",),
            ),
        ),
        source_addresses=(f"fixture:{name}",),
        revision=revision,
        verification_state="historical_only",
    )


def _fixture(
    endpoints=("experience:a", "shared"),
    outcomes=("success",),
    indirect=False,
    unknown=False,
):
    sources = tuple(
        _episode(
            f"experience:{chr(97 + i)}",
            "source-only",
            outcome,
            {"source_item_id": f"item:{i}"},
        )
        for i, outcome in enumerate(outcomes)
    )
    key = {"source_term": endpoints[0], "target_term": endpoints[1], "sign": 1}
    edge = _episode(
        "vrs-edge-group:7",
        "shared",
        "success",
        {**key, "canonical_group_id": 7},
        f"vrs-report-sha256:{CURRENT}",
    )
    extra = (
        (
            _episode(
                "vrs-edge-group:8",
                "shared",
                "pending",
                {
                    "source_term": "unknown",
                    "target_term": "shared",
                    "sign": 1,
                    "canonical_group_id": 8,
                },
                f"vrs-report-sha256:{CURRENT}",
            ),
        )
        if unknown
        else ()
    )
    # Unrelated pending experience stays in the same full memory in every arm.
    unrelated = _episode("pending", "unrelated", "pending", {"fixture": "pending"})
    memory = build_memory_activation_index((*sources, edge, *extra, unrelated))
    summary, relations = [], []
    if indirect:
        summary = [
            {
                **key,
                "edge_id": 7,
                "episode_ids": [s.episode_id for s in sources],
                "distinct_source_episode_count": len(sources),
            }
        ]
        relations = [
            {
                **key,
                "evidence": [
                    {
                        "episode_id": s.episode_id,
                        "revision": s.revision,
                        "source_addresses": list(s.source_addresses),
                        "source_item_id": s.steps[0].observation["source_item_id"],
                        "outcome": s.steps[0].outcome,
                    }
                    for s in sources
                ],
            }
        ]
    evidence = {
        "schema_version": "swegca-outcome-relation-evidence-v1",
        "relations": relations,
    }
    index = HotSourceProvenance.build(
        memory=memory,
        vrs_snapshot_id=CURRENT,
        relation_summary=summary,
        relation_evidence=evidence,
    )
    return memory, index, summary, evidence


def _activation(memory, overrides=None):
    overrides = overrides or {}
    return activate_memory(
        memory,
        query="fixture",
        current_cues=("shared",),
        judge=lambda row: CurrentEvidenceVerdict(
            row.episode_id,
            "current-p",
            overrides.get(row.episode_id, "support"),
            "synthetic-current-proof",
            ("fixture:current",),
        ),
    )


@pytest.mark.parametrize(
    "endpoints", [("experience:a", "shared"), ("shared", "experience:a")]
)
def test_direct_source_consumption_is_endpoint_orientation_independent(endpoints):
    memory, index, _, _ = _fixture(endpoints=endpoints)
    result = index.explain(memory, _activation(memory))
    assert result.decision == "success"
    assert result.contributors[0].sources[0].episode_id == "experience:a"
    assert not any(result.receipt()["authority"].values())


def test_indirect_complete_multisource_binding_is_not_a_causal_effect_claim():
    memory, index, _, _ = _fixture(
        endpoints=("profile", "outcome"), outcomes=("failure", "failure"), indirect=True
    )
    result = index.explain(memory, _activation(memory))
    assert result.decision == "failure"
    assert len(result.contributors[0].sources) == 2
    assert result.receipt()["source_count_is_causal_effect"] is False


@pytest.mark.parametrize(
    "outcome", ["success", "negative", "uncertain", "conflict", "pending"]
)
def test_conflicting_and_nonbinary_sources_abstain_without_discard(outcome):
    memory, index, _, _ = _fixture(
        endpoints=("profile", "outcome"), outcomes=("failure", outcome), indirect=True
    )
    result = index.explain(memory, _activation(memory))
    assert result.decision == "abstain"
    assert {s.outcome for s in result.contributors[0].sources} == {"failure", outcome}


def test_selected_unknown_blocks_decision_but_unselected_unknown_remains_accessible():
    memory, index, _, _ = _fixture(unknown=True)
    result = index.explain(memory, _activation(memory))
    assert result.decision == "abstain"
    assert result.contributors[1].unresolved_reason
    unselected = index.explain(
        memory, _activation(memory, {"vrs-edge-group:8": "insufficient"})
    )
    assert unselected.decision == "success"
    assert ("vrs-edge-group:8", "insufficient") in unselected.rejected_replays
    assert memory.episode("vrs-edge-group:8").steps[0].outcome == "pending"


@pytest.mark.parametrize("verdict", ["conflict", "refute"])
def test_current_refutation_and_conflict_are_not_overridden(verdict):
    memory, index, _, _ = _fixture(unknown=True)
    assert (
        index.explain(
            memory, _activation(memory, {"vrs-edge-group:8": verdict})
        ).decision
        == "abstain"
    )


@pytest.mark.parametrize(
    "field,value",
    [
        ("revision", "wrong"),
        ("outcome", "failure"),
        ("source_item_id", "wrong"),
        ("source_addresses", ["wrong"]),
    ],
)
def test_cold_binding_rejects_forged_source_provenance(field, value):
    memory, _, summary, evidence = _fixture(indirect=True)
    evidence["relations"][0]["evidence"][0][field] = value
    with pytest.raises(ValueError, match="provenance"):
        HotSourceProvenance.build(
            memory=memory,
            vrs_snapshot_id=CURRENT,
            relation_summary=summary,
            relation_evidence=evidence,
        )


def test_complete_relation_set_required_and_caller_mutation_detached():
    memory, index, summary, evidence = _fixture(indirect=True)
    evidence["relations"].clear()
    assert index.explain(memory, _activation(memory)).decision == "success"
    with pytest.raises(ValueError, match="complete summary"):
        HotSourceProvenance.build(
            memory=memory,
            vrs_snapshot_id=CURRENT,
            relation_summary=summary,
            relation_evidence=evidence,
        )
    with pytest.raises(TypeError):
        index.bindings[7] = None


def test_generation_and_replay_mismatch_fail_closed():
    memory, index, _, _ = _fixture()
    activation = _activation(memory)
    replay = activation.replay.episodes[0]
    assert index.resolve(
        replace(replay, source_addresses=("forged",))
    ).unresolved_reason
    other_generation = replace(index, vrs_snapshot_id=FROZEN)
    assert other_generation.explain(memory, activation).decision == "abstain"
    other_memory, _, _, _ = _fixture(endpoints=("shared", "experience:a"))
    with pytest.raises(ValueError, match="snapshot changed"):
        index.explain(other_memory, activation)


def test_existing_causal_engine_uses_hot_consumer_in_all_arms(monkeypatch):
    memory, index, _, _ = _fixture(endpoints=("profile", "outcome"), indirect=True)
    engine = HotCausalAblationEngine(
        pair=FullCurrentMemoryVrsSnapshot(memory, CURRENT),
        current_vrs=VrsPromotionProjection(CURRENT, {"vrs-edge-group:7": 1.2}),
        frozen_vrs=VrsPromotionProjection(FROZEN, {"vrs-edge-group:7": 0.8}),
        episode_roles=EpisodeRoleIndex({}, default_role="base"),
    )

    def forbidden(*_args, **_kwargs):
        raise AssertionError("hot consumer performed I/O or hashing")

    for obj, attr in ((builtins, "open"), (hashlib, "sha256"), (json, "dumps")):
        monkeypatch.setattr(obj, attr, forbidden)
    receipt = engine.evaluate(
        query="fixture",
        current_cues=("shared",),
        assess_current_evidence=lambda row: CurrentEvidenceVerdict(
            row.episode_id, "p", "support", "fixture proof", ("fixture:current",)
        ),
        decide=index.decide,
    )
    assert [arm.decision for arm in receipt.arms] == [
        "success",
        "abstain",
        "abstain",
        "success",
        "abstain",
    ]
    assert (
        len(
            {
                tuple(x.episode_id for x in arm.memory_activation.replay.episodes)
                for arm in receipt.arms
            }
        )
        == 1
    )
    assert memory.episode_ids_for_cue("unrelated") == ("pending",)
    for arm in receipt.arms:
        assert index.explain(memory, arm.memory_activation).decision == arm.decision


def test_selected_support_cannot_be_injected_outside_replay():
    memory, index, _, _ = _fixture()
    activation = _activation(memory)
    forged = replace(
        activation,
        re_evidence=replace(
            activation.re_evidence,
            selected_support=("vrs-edge-group:7", "absent"),
            judgments=(
                *activation.re_evidence.judgments,
                CurrentEvidenceVerdict(
                    "absent",
                    "current-p",
                    "support",
                    "forged fixture",
                    ("fixture:current",),
                ),
            ),
        ),
    )
    with pytest.raises(ValueError, match="inconsistent"):
        index.explain(memory, forged)


def test_direct_endpoint_cannot_silently_expand_complete_indirect_support():
    memory, index, _, _ = _fixture(
        endpoints=("experience:b", "shared"), outcomes=("success",), indirect=True
    )
    result = index.explain(memory, _activation(memory))
    assert result.decision == "abstain"
    assert (
        "absent from complete support ledger"
        in result.contributors[0].unresolved_reason
    )


def test_directed_relation_and_sign_are_not_reinterpreted():
    memory, index, _, _ = _fixture(indirect=True)
    key, sources = index.bindings[7]
    forged = replace(index, bindings={7: ((key[1], key[0], key[2]), sources)})
    assert forged.explain(memory, _activation(memory)).decision == "abstain"
    changed_sign = replace(index, bindings={7: ((key[0], key[1], -1), sources)})
    assert changed_sign.explain(memory, _activation(memory)).decision == "abstain"


def test_indirect_and_mixed_roles_are_not_mislabeled_pure_base():
    memory, index, _, _ = _fixture(
        endpoints=("profile", "outcome"), outcomes=("failure", "failure"), indirect=True
    )
    assert (
        ProvenanceEpisodeRoles(index, frozenset({"experience:a", "experience:b"})).role(
            "vrs-edge-group:7"
        )
        == "repair"
    )
    assert ProvenanceEpisodeRoles(index, frozenset()).role("vrs-edge-group:7") == "base"
    roles = ProvenanceEpisodeRoles(index, frozenset({"experience:a"}))
    assert roles.role("vrs-edge-group:7") == "other"
    engine = HotCausalAblationEngine(
        pair=FullCurrentMemoryVrsSnapshot(memory, CURRENT),
        current_vrs=VrsPromotionProjection(CURRENT, {"vrs-edge-group:7": 1.2}),
        frozen_vrs=VrsPromotionProjection(FROZEN, {"vrs-edge-group:7": 0.8}),
        episode_roles=roles,
    )
    result = engine.evaluate(
        query="fixture",
        current_cues=("shared",),
        assess_current_evidence=lambda row: CurrentEvidenceVerdict(
            row.episode_id, "p", "support", "fixture proof", ("fixture:current",)
        ),
        decide=index.decide,
    )
    assert [arm.decision for arm in result.arms] == [
        "failure",
        "abstain",
        "abstain",
        "abstain",
        "abstain",
    ]
