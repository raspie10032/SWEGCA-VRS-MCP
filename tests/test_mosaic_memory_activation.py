from __future__ import annotations

import builtins

import pytest

import swegca_vrs_mcp.core.mosaic_memory_activation as memory_activation
from swegca_vrs_mcp.core.mosaic_memory_activation import (
    AtomicFullCurrentMemoryVrsOwner,
    AtomicMemoryActivationOwner,
    CompositeMemoryActivationIndex,
    CurrentEvidenceVerdict,
    DejaVuSignal,
    FullCurrentMemoryVrsSnapshot,
    MemoryEpisode,
    MemoryStep,
    ReplayResult,
    activate_memory,
    build_memory_activation_index,
    detect_deja_vu,
    recall_memory,
    replay_memory,
)


def _episode(
    episode_id: str,
    *,
    cues: tuple[str, ...],
    outcome: str,
    judgment: str,
) -> MemoryEpisode:
    return MemoryEpisode(
        episode_id=episode_id,
        cues=cues,
        steps=(
            MemoryStep(
                phase="observation_to_outcome",
                observation={"state": "closed", "target": "door"},
                relations=("actor-near-door", "door-before-action"),
                judgment=judgment,
                outcome=outcome,
                evidence_refs=(
                    f"frame:{episode_id}:before",
                    f"frame:{episode_id}:after",
                ),
            ),
        ),
        source_addresses=(f"experience:{episode_id}",),
        revision="r1",
        verification_state="historical_only",
    )


def _index():
    return build_memory_activation_index(
        (
            _episode(
                "success",
                cues=("door", "closed"),
                outcome="success",
                judgment="interact opens door",
            ),
            _episode(
                "failure",
                cues=("door", "closed"),
                outcome="failure",
                judgment="interact had no effect",
            ),
            _episode(
                "uncertain",
                cues=("door", "dark"),
                outcome="uncertain",
                judgment="state unreadable",
            ),
            _episode(
                "conflict",
                cues=("door", "closed"),
                outcome="conflict",
                judgment="two observations oppose",
            ),
            _episode(
                "pending",
                cues=("door", "closed"),
                outcome="pending",
                judgment="current evidence request remains unresolved",
            ),
        )
    )


def test_deja_vu_is_anonymous_trigger_before_recall() -> None:
    signal = detect_deja_vu(
        _index(), query="문 앞", current_cues=("door", "closed", "night")
    )
    assert signal.triggered
    assert signal.candidate_count == 5
    assert not signal.memory_identifiers_exposed
    assert not hasattr(signal, "episode_ids")


def test_recall_keeps_every_historical_outcome_without_allowlist() -> None:
    index = _index()
    recalled = recall_memory(
        index,
        detect_deja_vu(index, query="문 앞", current_cues=("door", "closed")),
    )
    outcomes = {
        outcome
        for candidate in recalled.candidates
        for outcome in candidate.historical_outcomes
    }
    assert outcomes == {"success", "failure", "uncertain", "conflict", "pending"}
    assert not recalled.codex_per_item_allowlist_used


def test_replay_preserves_original_flow_and_is_not_truth() -> None:
    index = _index()
    recalled = recall_memory(
        index, detect_deja_vu(index, query="q", current_cues=("door",))
    )
    replayed = replay_memory(index, recalled)
    assert replayed.episodes
    assert all(not episode.historical_truth_authorized for episode in replayed.episodes)
    assert replayed.episodes[0].steps[0].relations


def test_composite_hot_router_preserves_all_sources_and_cue_postings() -> None:
    first = build_memory_activation_index(
        (_episode("first", cues=("shared", "left"), outcome="failure", judgment="a"),)
    )
    second = build_memory_activation_index(
        (_episode("second", cues=("shared", "right"), outcome="success", judgment="b"),)
    )
    composite = CompositeMemoryActivationIndex((first, second))

    assert composite.episode("first") is first.episode("first")
    assert composite.episode("second") is second.episode("second")
    assert composite.episode_ids_for_cue(" SHARED ") == ("first", "second")
    assert composite.episode_ids_for_cue("left") == ("first",)
    assert composite.episode_count == 2
    assert set(composite.iter_episode_ids()) == {"first", "second"}


def test_reevidence_can_refute_prior_success_and_forces_conflict_abstention() -> None:
    index = _index()

    def judge(episode):
        verdict = "support" if episode.episode_id == "failure" else "refute"
        return CurrentEvidenceVerdict(
            episode.episode_id,
            "door-opens",
            verdict,
            "현재 프레임 전후 변화로 다시 판정함",
            ("current:before", "current:after"),
        )

    receipt = activate_memory(
        index, query="문이 열리는가", current_cues=("door", "closed"), judge=judge
    )
    assert "success" in receipt.re_evidence.selected_refutation
    assert receipt.re_evidence.unresolved_conflict
    assert not receipt.re_evidence.insufficient_evidence
    assert receipt.re_evidence.should_abstain
    assert not receipt.action_authorized


def test_hot_activation_performs_no_file_io(monkeypatch: pytest.MonkeyPatch) -> None:
    index = _index()

    def forbidden_open(*_args, **_kwargs):
        raise AssertionError("hot memory activation attempted file I/O")

    monkeypatch.setattr(builtins, "open", forbidden_open)
    monkeypatch.setattr(memory_activation.hashlib, "sha256", forbidden_open)
    monkeypatch.setattr(memory_activation.json, "dumps", forbidden_open)

    def judge(episode):
        return CurrentEvidenceVerdict(
            episode.episode_id,
            "door-opens",
            "insufficient",
            "현재 증거가 부족함",
            ("current:frame",),
        )

    receipt = activate_memory(index, query="문", current_cues=("door",), judge=judge)
    assert receipt.stage_order == ("deja_vu", "recall", "replay", "re_evidence")
    assert not receipt.persistent_write_authorized


def test_memory_episode_observation_is_detached_from_mutable_input() -> None:
    observation = {"nested": {"value": 1}}
    step = MemoryStep(
        "observe",
        observation,
        ("a-before-b",),
        "unknown",
        "uncertain",
        ("frame:a",),
    )
    observation["nested"]["value"] = 9
    assert step.observation["nested"]["value"] == 1
    with pytest.raises(TypeError):
        step.observation["nested"]["value"] = 2


def test_no_deja_vu_match_stops_with_empty_current_evidence() -> None:
    calls = 0

    def judge(_episode):
        nonlocal calls
        calls += 1
        raise AssertionError("an unmatched memory must not be judged")

    receipt = activate_memory(
        _index(), query="처음 보는 구조", current_cues=("river",), judge=judge
    )
    assert not receipt.deja_vu.triggered
    assert receipt.recall.candidates == ()
    assert receipt.replay.episodes == ()
    assert receipt.re_evidence.judgments == ()
    assert calls == 0


def test_all_insufficient_re_evidence_abstains_without_current_evidence() -> None:
    index = _index()

    def judge(episode):
        return CurrentEvidenceVerdict(
            episode.episode_id,
            "door-opens",
            "insufficient",
            "현재 증거가 없음",
            (),
        )

    receipt = activate_memory(
        index, query="문이 열리는가", current_cues=("door",), judge=judge
    )
    assert receipt.re_evidence.insufficient_evidence
    assert receipt.re_evidence.should_abstain
    assert not receipt.re_evidence.unresolved_conflict


def test_current_support_still_requires_current_evidence() -> None:
    with pytest.raises(ValueError, match="support or refute requires current evidence"):
        CurrentEvidenceVerdict(
            "episode",
            "door-opens",
            "support",
            "현재 증거가 없음",
            (),
        )


def test_support_and_refutation_only_conflict_for_same_proposition() -> None:
    index = _index()

    def unrelated(episode):
        return CurrentEvidenceVerdict(
            episode.episode_id,
            "door-opens" if episode.episode_id == "success" else "lamp-turns-on",
            "support" if episode.episode_id == "success" else "refute",
            "현재 증거로 서로 다른 명제를 판정함",
            ("current:frame",),
        )

    receipt = activate_memory(
        index, query="장면 판단", current_cues=("door", "closed"), judge=unrelated
    )
    assert not receipt.re_evidence.unresolved_conflict
    assert not receipt.re_evidence.should_abstain


def test_empty_hot_index_is_a_valid_no_memory_state() -> None:
    index = build_memory_activation_index(())
    signal = detect_deja_vu(index, query="첫 관측", current_cues=("unknown",))
    assert not signal.triggered
    assert index.episodes_by_id == {}
    assert not any(
        (
            index.includes_success,
            index.includes_failure,
            index.includes_negative,
            index.includes_uncertain,
            index.includes_conflict,
            index.includes_pending,
        )
    )


def test_required_outcome_coverage_is_derived_from_actual_episodes() -> None:
    complete = build_memory_activation_index(
        _index().episodes_by_id.values(),
        required_outcomes={"success", "failure", "uncertain", "conflict", "pending"},
    )
    assert complete.includes_pending

    with pytest.raises(ValueError, match="missing required historical outcomes"):
        build_memory_activation_index(
            (
                _episode(
                    "only-success", cues=("door",), outcome="success", judgment="ok"
                ),
            ),
            required_outcomes={
                "success",
                "failure",
                "uncertain",
                "conflict",
                "pending",
            },
        )


def test_negative_experience_is_an_active_hot_snapshot_input() -> None:
    negative = _episode(
        "negative",
        cues=("door", "closed"),
        outcome="negative",
        judgment="the attempted relation was explicitly absent",
    )
    index = build_memory_activation_index(
        (*_index().episodes_by_id.values(), negative),
        required_outcomes={
            "success",
            "failure",
            "negative",
            "uncertain",
            "conflict",
            "pending",
        },
    )
    recalled = recall_memory(
        index,
        detect_deja_vu(index, query="문 앞", current_cues=("door", "closed")),
    )
    assert "negative" in {
        outcome
        for candidate in recalled.candidates
        for outcome in candidate.historical_outcomes
    }
    assert index.includes_negative


def test_snapshot_and_authority_tamper_fail_closed() -> None:
    index = _index()
    signal = detect_deja_vu(index, query="q", current_cues=("door",))
    changed = DejaVuSignal(
        "f" * 64,
        signal.query,
        signal.current_cues,
        signal.matched_cues,
        signal.recognition_strength,
        signal.candidate_count,
    )
    with pytest.raises(ValueError, match="snapshot changed"):
        recall_memory(index, changed)
    with pytest.raises(ValueError, match="replay grants no authority"):
        ReplayResult("q", (), action_authorized=True)


def test_invalid_authority_and_filtered_indexes_fail_closed() -> None:
    index = _index()
    with pytest.raises(ValueError):
        type(index)(
            index.snapshot_id,
            index.episodes_by_id,
            index.postings_by_cue,
            includes_failure=False,
        )
    with pytest.raises(ValueError, match="snapshot content changed"):
        type(index)("f" * 64, index.episodes_by_id, index.postings_by_cue)


def test_main_owner_atomically_replaces_one_full_hot_snapshot() -> None:
    initial = _index()
    replacement = build_memory_activation_index(
        (
            *initial.episodes_by_id.values(),
            _episode("new", cues=("river",), outcome="pending", judgment="awaiting"),
        )
    )
    owner = AtomicMemoryActivationOwner(initial)

    held = owner.snapshot()
    assert owner.replace(held.snapshot_id, replacement) == replacement.snapshot_id
    assert owner.snapshot() is replacement
    assert held is initial
    with pytest.raises(ValueError, match="changed before replacement"):
        owner.replace(initial.snapshot_id, initial)


def test_cold_build_computes_the_content_snapshot_digest_once(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    original = memory_activation._snapshot_id
    calls = 0

    def counted(*args, **kwargs):
        nonlocal calls
        calls += 1
        return original(*args, **kwargs)

    monkeypatch.setattr(memory_activation, "_snapshot_id", counted)
    index = build_memory_activation_index(
        (_episode("one", cues=("door",), outcome="success", judgment="opened"),)
    )

    assert index.snapshot_id
    assert calls == 1


def test_main_atomically_replaces_one_memory_and_vrs_pair() -> None:
    initial_memory = _index()
    replacement_memory = build_memory_activation_index(
        (
            *initial_memory.episodes_by_id.values(),
            _episode("new", cues=("river",), outcome="pending", judgment="awaiting"),
        )
    )
    initial = FullCurrentMemoryVrsSnapshot(initial_memory, "a" * 64)
    replacement = FullCurrentMemoryVrsSnapshot(replacement_memory, "b" * 64)
    owner = AtomicFullCurrentMemoryVrsOwner(initial)

    held = owner.snapshot()
    assert owner.replace(held.snapshot_id, replacement) == replacement.snapshot_id
    assert owner.snapshot() is replacement
    assert owner.snapshot().memory is replacement_memory
    assert owner.snapshot().vrs_snapshot_id == "b" * 64
    assert held.memory is initial_memory
    assert held.vrs_snapshot_id == "a" * 64
    with pytest.raises(ValueError, match="changed before replacement"):
        owner.replace(initial.snapshot_id, initial)


def test_full_current_pair_digest_binds_both_memory_and_vrs() -> None:
    memory = _index()
    changed_memory = build_memory_activation_index(
        (
            *memory.episodes_by_id.values(),
            _episode("new", cues=("new",), outcome="success", judgment="new"),
        )
    )

    baseline = FullCurrentMemoryVrsSnapshot(memory, "a" * 64)
    assert (
        FullCurrentMemoryVrsSnapshot(changed_memory, "a" * 64).snapshot_id
        != baseline.snapshot_id
    )
    assert (
        FullCurrentMemoryVrsSnapshot(memory, "b" * 64).snapshot_id
        != baseline.snapshot_id
    )


def test_runtime_cue_selection_uses_the_smallest_nonempty_hot_fanout() -> None:
    index = build_memory_activation_index(
        (
            _episode(
                "one", cues=("broad", "specific"), outcome="success", judgment="one"
            ),
            _episode("two", cues=("broad",), outcome="failure", judgment="two"),
        )
    )

    selection = memory_activation.select_runtime_cues(
        index,
        query="Which prior experience is most specific?",
        candidate_cues=("missing", "broad", "specific"),
    )

    assert selection.selected_cues == ("specific",)
    assert selection.candidate_counts == {"missing": 0, "broad": 2, "specific": 1}
    assert selection.codex_or_evaluator_allowlist_used is False


def test_runtime_cue_selection_prioritizes_current_evidence_semantics() -> None:
    index = build_memory_activation_index(
        (
            _episode(
                "exact-one",
                cues=("current-signature",),
                outcome="failure",
                judgment="one",
            ),
            _episode(
                "exact-two",
                cues=("current-signature",),
                outcome="failure",
                judgment="two",
            ),
            _episode(
                "generic-only",
                cues=("generic",),
                outcome="success",
                judgment="three",
            ),
        )
    )

    selection = memory_activation.select_runtime_cues(
        index,
        query="Judge the current worker signature",
        candidate_cues=("current-signature", "generic"),
        preferred_current_evidence_cues=("current-signature",),
    )

    assert selection.candidate_counts == {"current-signature": 2, "generic": 1}
    assert selection.selected_cues == ("current-signature",)
    assert (
        selection.selection_method
        == "current_evidence_semantic_priority_then_hot_fanout"
    )
    assert selection.codex_or_evaluator_allowlist_used is False
