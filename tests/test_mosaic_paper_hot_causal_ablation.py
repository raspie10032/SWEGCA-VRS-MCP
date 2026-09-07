from __future__ import annotations

import builtins
import hashlib
import json
from collections.abc import Mapping

import pytest

import swegca_vrs_mcp.core.mosaic_memory_activation as memory_activation
import swegca_vrs_mcp.core.mosaic_paper_hot_causal_ablation as causal
from swegca_vrs_mcp.core.mosaic_memory_activation import (
    CurrentEvidenceVerdict,
    FullCurrentMemoryVrsSnapshot,
    MemoryEpisode,
    MemoryStep,
    build_memory_activation_index,
)
from swegca_vrs_mcp.core.mosaic_paper_hot_causal_ablation import (
    ARMS,
    CompletedUnitCache,
    EpisodeRoleIndex,
    HotCausalAblationEngine,
    VrsPromotionProjection,
    acquire_visual_outcome_once,
)

CURRENT_VRS = "1" * 64
FROZEN_VRS = "2" * 64


def _episode(episode_id: str, *, cue: str, outcome: str) -> MemoryEpisode:
    return MemoryEpisode(
        episode_id=episode_id,
        cues=(cue, "paper-causal"),
        steps=(
            MemoryStep(
                phase="observation_to_outcome",
                observation={"episode": episode_id},
                relations=("current-evidence-compatible",),
                judgment=f"historical {outcome}",
                outcome=outcome,
                evidence_refs=(f"fixture:{episode_id}",),
            ),
        ),
        source_addresses=(f"experience:{episode_id}",),
        revision="synthetic-r1",
        verification_state="historical_only_pending_current_reevidence",
    )


def _engine() -> HotCausalAblationEngine:
    memory = build_memory_activation_index(
        (
            _episode("base-success", cue="shared", outcome="success"),
            _episode("repair-failure", cue="shared", outcome="failure"),
            _episode("negative", cue="other-negative", outcome="negative"),
            _episode("uncertain", cue="other-uncertain", outcome="uncertain"),
            _episode("conflict", cue="other-conflict", outcome="conflict"),
            _episode("pending", cue="other-pending", outcome="pending"),
        ),
        required_outcomes=(
            "success",
            "failure",
            "negative",
            "uncertain",
            "conflict",
            "pending",
        ),
    )
    pair = FullCurrentMemoryVrsSnapshot(memory, CURRENT_VRS)
    current = VrsPromotionProjection(
        CURRENT_VRS,
        {"base-success": 1.2, "repair-failure": 0.8},
    )
    frozen = VrsPromotionProjection(
        FROZEN_VRS,
        {"base-success": 0.8, "repair-failure": 1.2},
    )
    return HotCausalAblationEngine(
        pair=pair,
        current_vrs=current,
        frozen_vrs=frozen,
        episode_roles=EpisodeRoleIndex(
            {"repair-failure": "repair"}, default_role="base"
        ),
    )


def _support(episode) -> CurrentEvidenceVerdict:
    return CurrentEvidenceVerdict(
        episode_id=episode.episode_id,
        proposition=f"outcome-compatible:{episode.episode_id}",
        verdict="support",
        rationale="synthetic current evidence is compatible",
        current_evidence_refs=("fixture:current-observation",),
    )


def _decision(memory, activation) -> str:
    selected = activation.re_evidence.selected_support
    outcomes = {memory.episode(episode_id).steps[0].outcome for episode_id in selected}
    return next(iter(outcomes)) if len(outcomes) == 1 else "abstain"


def test_vrs_strength_changes_the_actual_reevidence_and_decision_path() -> None:
    receipt = _engine().evaluate(
        query="predict later stability",
        current_cues=("shared",),
        assess_current_evidence=_support,
        decide=_decision,
    )
    by_arm = {row.arm: row for row in receipt.arms}

    assert tuple(by_arm) == ARMS
    assert by_arm["full_current_current_vrs"].decision == "success"
    assert by_arm["full_current_frozen_vrs"].decision == "failure"
    assert by_arm["full_current_no_vrs"].decision == "abstain"
    assert not by_arm["full_current_no_vrs"].decision_rule_invoked
    assert by_arm["full_current_current_vrs_base_promotion_only"].decision == "success"
    assert (
        by_arm["full_current_current_vrs_repair_promotion_only"].decision == "abstain"
    )
    assert by_arm["full_current_current_vrs"].effective_vrs_snapshot_id == CURRENT_VRS
    assert by_arm["full_current_frozen_vrs"].effective_vrs_snapshot_id == FROZEN_VRS


def test_every_arm_retains_identical_full_current_recall() -> None:
    receipt = _engine().evaluate(
        query="same query",
        current_cues=("shared",),
        assess_current_evidence=_support,
        decide=_decision,
    )
    recalled = {
        tuple(row.episode_id for row in arm.memory_activation.recall.candidates)
        for arm in receipt.arms
    }
    replayed = {
        tuple(row.episode_id for row in arm.memory_activation.replay.episodes)
        for arm in receipt.arms
    }
    assert recalled == {("base-success", "repair-failure")}
    assert replayed == recalled
    assert all(
        arm.memory_activation.snapshot_id == receipt.memory_snapshot_id
        for arm in receipt.arms
    )


def test_common_deja_vu_recall_replay_run_once_per_request(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    counts = {"deja_vu": 0, "recall": 0, "replay": 0}

    def wrap(name, original):
        def counted(*args, **kwargs):
            counts[name] += 1
            return original(*args, **kwargs)

        return counted

    monkeypatch.setattr(
        causal,
        "detect_deja_vu",
        wrap("deja_vu", causal.detect_deja_vu),
    )
    monkeypatch.setattr(
        causal,
        "recall_memory",
        wrap("recall", causal.recall_memory),
    )
    monkeypatch.setattr(
        causal,
        "replay_memory",
        wrap("replay", causal.replay_memory),
    )
    receipt = _engine().evaluate(
        query="one shared spine",
        current_cues=("shared",),
        assess_current_evidence=_support,
        decide=_decision,
    )

    assert counts == {"deja_vu": 1, "recall": 1, "replay": 1}
    assert tuple(receipt.common_stage_intervals) == (
        "deja_vu",
        "recall",
        "replay",
    )
    assert all(
        interval.elapsed_ns >= 0 for interval in receipt.common_stage_intervals.values()
    )
    assert all(row.re_evidence_interval.elapsed_ns >= 0 for row in receipt.arms)


def test_hot_evaluate_performs_no_io_json_network_or_hash_work(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    engine = _engine()

    def forbidden(*_args, **_kwargs):
        raise AssertionError("hot causal request attempted a forbidden cold operation")

    monkeypatch.setattr(builtins, "open", forbidden)
    monkeypatch.setattr(json, "dumps", forbidden)
    monkeypatch.setattr(hashlib, "sha256", forbidden)
    monkeypatch.setattr(memory_activation.json, "dumps", forbidden)
    monkeypatch.setattr(memory_activation.hashlib, "sha256", forbidden)

    first = engine.evaluate(
        query="hot request one",
        current_cues=("shared",),
        assess_current_evidence=_support,
        decide=_decision,
    )
    second = engine.evaluate(
        query="hot request two",
        current_cues=("shared",),
        assess_current_evidence=_support,
        decide=_decision,
    )
    assert first.cold_bootstrap_count == second.cold_bootstrap_count == 1
    assert first.full_current_rebuilds_this_request == 0
    assert second.full_current_rebuilds_this_request == 0
    assert engine.pair.memory is engine.pair.memory
    assert engine.request_count == 2


def test_reevidence_abstention_prevents_decision_callback() -> None:
    engine = _engine()
    calls = 0

    def conflict(episode) -> CurrentEvidenceVerdict:
        verdict = "support" if episode.episode_id == "base-success" else "refute"
        return CurrentEvidenceVerdict(
            episode_id=episode.episode_id,
            proposition="same-proposition",
            verdict=verdict,
            rationale="synthetic opposing current evidence",
            current_evidence_refs=(f"fixture:{verdict}",),
        )

    def forbidden_decision(_memory, _activation) -> str:
        nonlocal calls
        calls += 1
        return "failure"

    # Promote both episodes in both real projections so the full arms conflict.
    memory = engine.pair.memory
    conflicting = HotCausalAblationEngine(
        pair=engine.pair,
        current_vrs=VrsPromotionProjection(
            CURRENT_VRS,
            {"base-success": 1.2, "repair-failure": 1.2},
        ),
        frozen_vrs=VrsPromotionProjection(
            FROZEN_VRS,
            {"base-success": 1.2, "repair-failure": 1.2},
        ),
        episode_roles=EpisodeRoleIndex({"repair-failure": "repair"}),
    )
    receipt = conflicting.evaluate(
        query="conflicted proposition",
        current_cues=("shared",),
        assess_current_evidence=conflict,
        decide=forbidden_decision,
    )
    by_arm = {row.arm: row for row in receipt.arms}
    assert memory is conflicting.pair.memory
    assert by_arm["full_current_current_vrs"].decision == "abstain"
    assert by_arm["full_current_frozen_vrs"].decision == "abstain"
    assert by_arm[
        "full_current_current_vrs"
    ].memory_activation.re_evidence.should_abstain
    assert by_arm[
        "full_current_frozen_vrs"
    ].memory_activation.re_evidence.should_abstain
    # Base-only and repair-only remain non-conflicted, so only those invoke it.
    assert calls == 2


def test_visual_only_checkpoint_skips_completed_reacquisition_and_inference() -> None:
    calls = 0
    checkpoint = CompletedUnitCache()

    def visual_worker(value: object) -> Mapping[str, object]:
        nonlocal calls
        calls += 1
        return {"proposal": "stable", "input": value}

    first = acquire_visual_outcome_once(
        unit_id="K101",
        visual_input="frame-delta",
        checkpoint=checkpoint,
        visual_worker=visual_worker,
    )
    second = acquire_visual_outcome_once(
        unit_id="K101",
        visual_input="must-not-be-read",
        checkpoint=checkpoint,
        visual_worker=visual_worker,
    )

    assert calls == 1
    assert not first.checkpoint_hit and first.visual_worker_calls == 1
    assert second.checkpoint_hit and second.visual_worker_calls == 0
    assert second.outcome == first.outcome
    assert second.asr_worker_calls == second.ocr_worker_calls == 0


def test_projection_and_pair_binding_fail_closed() -> None:
    engine = _engine()
    with pytest.raises(ValueError, match="not bound"):
        HotCausalAblationEngine(
            pair=engine.pair,
            current_vrs=VrsPromotionProjection("3" * 64, {}),
            frozen_vrs=VrsPromotionProjection(FROZEN_VRS, {}),
            episode_roles=EpisodeRoleIndex({}),
        )
    with pytest.raises(ValueError, match="distinct"):
        HotCausalAblationEngine(
            pair=engine.pair,
            current_vrs=VrsPromotionProjection(CURRENT_VRS, {}),
            frozen_vrs=VrsPromotionProjection(CURRENT_VRS, {}),
            episode_roles=EpisodeRoleIndex({}),
        )


def test_small_projection_records_a_content_digest() -> None:
    first = VrsPromotionProjection(CURRENT_VRS, {"episode": 1.0})
    same = VrsPromotionProjection(CURRENT_VRS, {"episode": 1.0})
    changed = VrsPromotionProjection(CURRENT_VRS, {"episode": 0.99})
    assert first.projection_sha256 == same.projection_sha256
    assert first.projection_sha256 != changed.projection_sha256
