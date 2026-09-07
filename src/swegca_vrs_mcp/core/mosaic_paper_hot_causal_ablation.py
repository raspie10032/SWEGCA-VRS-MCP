"""Hot, VRS-bound causal ablation primitives for the SWEGCA/VRS paper.

This module owns no identity, persistent cognition, experience, model, VRS, or
consequential authority.  A main-owned immutable full-current pair and VRS
promotion projections are constructed on a cold path and passed in by handle.
Every hot request shares one Deja vu -> Recall -> Replay spine across diagnostic
arms and varies only the effective VRS promotion evidence at Re-evidence.
"""

from __future__ import annotations

import hashlib
import json
import re
import time
from collections.abc import Callable, Iterable, Mapping
from dataclasses import dataclass, field
from threading import Lock
from types import MappingProxyType
from typing import Any, Literal, Protocol, runtime_checkable

from swegca_vrs_mcp.core.mosaic_memory_activation import (
    CurrentEvidenceVerdict,
    FullCurrentMemoryVrsSnapshot,
    HotMemoryIndex,
    MemoryActivationReceipt,
    ReplayedEpisode,
    detect_deja_vu,
    re_evidence_memory,
    recall_memory,
    replay_memory,
)

EpisodeRole = Literal["base", "repair", "other"]
ArmName = Literal[
    "full_current_current_vrs",
    "full_current_frozen_vrs",
    "full_current_no_vrs",
    "full_current_current_vrs_base_promotion_only",
    "full_current_current_vrs_repair_promotion_only",
]

ARMS: tuple[ArmName, ...] = (
    "full_current_current_vrs",
    "full_current_frozen_vrs",
    "full_current_no_vrs",
    "full_current_current_vrs_base_promotion_only",
    "full_current_current_vrs_repair_promotion_only",
)
PROMOTION_THRESHOLD = 1.0
AUTHORITY_FALSE = MappingProxyType(
    {
        "semantic": False,
        "world": False,
        "action": False,
        "persistent_write": False,
        "model_update": False,
        "distribution": False,
        "p3": False,
    }
)

EvidenceAssessor = Callable[[ReplayedEpisode], CurrentEvidenceVerdict]
DecisionRule = Callable[[HotMemoryIndex, MemoryActivationReceipt], str]
VisualWorker = Callable[[Any], Mapping[str, Any]]


def _digest(value: str, label: str) -> str:
    normalized = str(value).strip().lower()
    if not re.fullmatch(r"[0-9a-f]{64}", normalized):
        raise ValueError(f"{label} must be a SHA-256 digest")
    return normalized


@runtime_checkable
class HotVrsStrengthIndex(Protocol):
    """Minimum resident VRS surface; a large graph need not become a new dict."""

    snapshot_id: str
    lookup_requires_io: bool

    def strength(self, episode_id: str) -> float: ...


@dataclass(frozen=True, slots=True)
class VrsPromotionProjection:
    """Small cold-built VRS projection used by fixtures and incremental waves."""

    snapshot_id: str
    strengths_by_episode_id: Mapping[str, float]
    lookup_requires_io: bool = False
    projection_sha256: str = field(init=False)

    def __post_init__(self) -> None:
        object.__setattr__(
            self, "snapshot_id", _digest(self.snapshot_id, "VRS snapshot")
        )
        strengths = {
            str(episode_id): float(strength)
            for episode_id, strength in self.strengths_by_episode_id.items()
        }
        if any(not episode_id.strip() for episode_id in strengths):
            raise ValueError("VRS projection contains an empty episode ID")
        if any(strength < 0 for strength in strengths.values()):
            raise ValueError("VRS strength must be non-negative")
        if self.lookup_requires_io:
            raise ValueError("hot VRS projection cannot require I/O")
        object.__setattr__(
            self,
            "strengths_by_episode_id",
            MappingProxyType(strengths),
        )
        payload = json.dumps(
            {
                "schema_version": "swegca-hot-vrs-strength-projection-v1",
                "source_vrs_snapshot_id": self.snapshot_id,
                "strengths_by_episode_id": strengths,
            },
            sort_keys=True,
            separators=(",", ":"),
        ).encode("utf-8")
        object.__setattr__(
            self, "projection_sha256", hashlib.sha256(payload).hexdigest()
        )

    def strength(self, episode_id: str) -> float:
        return self.strengths_by_episode_id.get(episode_id, 0.0)


@dataclass(frozen=True, slots=True)
class EpisodeRoleIndex:
    """Provenance-derived role lookup; this is not a retrieval allowlist."""

    overrides: Mapping[str, EpisodeRole]
    default_role: EpisodeRole = "base"
    codex_or_evaluator_allowlist_used: bool = False

    def __post_init__(self) -> None:
        if self.default_role not in {"base", "repair", "other"}:
            raise ValueError("default episode role changed")
        roles = {str(key): value for key, value in self.overrides.items()}
        if any(not key.strip() for key in roles) or any(
            value not in {"base", "repair", "other"} for value in roles.values()
        ):
            raise ValueError("episode role index changed")
        if self.codex_or_evaluator_allowlist_used:
            raise ValueError("episode role metadata became a retrieval allowlist")
        object.__setattr__(self, "overrides", MappingProxyType(roles))

    def role(self, episode_id: str) -> EpisodeRole:
        return self.overrides.get(episode_id, self.default_role)


@dataclass(frozen=True, slots=True)
class StageInterval:
    started_monotonic_ns: int
    ended_monotonic_ns: int
    elapsed_ns: int = field(init=False)

    def __post_init__(self) -> None:
        if self.ended_monotonic_ns < self.started_monotonic_ns:
            raise ValueError("stage clock moved backwards")
        object.__setattr__(
            self,
            "elapsed_ns",
            self.ended_monotonic_ns - self.started_monotonic_ns,
        )

    def receipt(self) -> dict[str, int]:
        return {
            "started_monotonic_ns": self.started_monotonic_ns,
            "ended_monotonic_ns": self.ended_monotonic_ns,
            "elapsed_ns": self.elapsed_ns,
        }


@dataclass(frozen=True, slots=True)
class CausalArmResult:
    arm: ArmName
    effective_vrs_snapshot_id: str | None
    promotion_roles: tuple[EpisodeRole, ...]
    memory_activation: MemoryActivationReceipt
    decision: str
    decisive: bool
    decision_rule_invoked: bool
    re_evidence_interval: StageInterval

    def __post_init__(self) -> None:
        if self.arm not in ARMS:
            raise ValueError("causal arm changed")
        if not self.decision.strip():
            raise ValueError("causal decision is empty")
        should_abstain = self.memory_activation.re_evidence.should_abstain
        if should_abstain and (
            self.decision != "abstain" or self.decisive or self.decision_rule_invoked
        ):
            raise ValueError("Re-evidence abstention was bypassed")

    def receipt(self) -> dict[str, Any]:
        activation = self.memory_activation
        return {
            "arm": self.arm,
            "effective_vrs_snapshot_id": self.effective_vrs_snapshot_id,
            "promotion_roles": list(self.promotion_roles),
            "memory_snapshot_id": activation.snapshot_id,
            "stage_order": list(activation.stage_order),
            "recalled_episode_ids": [
                row.episode_id for row in activation.recall.candidates
            ],
            "replayed_episode_ids": [
                row.episode_id for row in activation.replay.episodes
            ],
            "selected_support": list(activation.re_evidence.selected_support),
            "selected_refutation": list(activation.re_evidence.selected_refutation),
            "conflicting_propositions": list(
                activation.re_evidence.conflicting_propositions
            ),
            "should_abstain": activation.re_evidence.should_abstain,
            "decision": self.decision,
            "decisive": self.decisive,
            "decision_rule_invoked": self.decision_rule_invoked,
            "re_evidence_interval": self.re_evidence_interval.receipt(),
            "full_current_experience_addressability_preserved": True,
            "codex_or_evaluator_allowlist_used": False,
            "authority": dict(AUTHORITY_FALSE),
        }


@dataclass(frozen=True, slots=True)
class CausalEvaluationReceipt:
    pair_snapshot_id: str
    memory_snapshot_id: str
    common_stage_intervals: Mapping[str, StageInterval]
    arms: tuple[CausalArmResult, ...]
    cold_bootstrap_count: int
    request_sequence: int
    full_current_rebuilds_this_request: int = 0

    def __post_init__(self) -> None:
        if tuple(self.common_stage_intervals) != ("deja_vu", "recall", "replay"):
            raise ValueError("common memory activation stage order changed")
        if tuple(row.arm for row in self.arms) != ARMS:
            raise ValueError("causal arm order changed")
        if self.cold_bootstrap_count != 1 or self.full_current_rebuilds_this_request:
            raise ValueError("hot request rebuilt full-current cognition")
        recalled = {
            tuple(
                candidate.episode_id
                for candidate in row.memory_activation.recall.candidates
            )
            for row in self.arms
        }
        if len(recalled) != 1:
            raise ValueError("diagnostic arm changed memory addressability")
        if any(
            row.memory_activation.snapshot_id != self.memory_snapshot_id
            for row in self.arms
        ):
            raise ValueError("causal arms did not share one memory snapshot")

    def receipt(self) -> dict[str, Any]:
        return {
            "schema_version": "swegca-paper-hot-causal-evaluation-v1",
            "pair_snapshot_id": self.pair_snapshot_id,
            "memory_snapshot_id": self.memory_snapshot_id,
            "common_stage_intervals": {
                name: interval.receipt()
                for name, interval in self.common_stage_intervals.items()
            },
            "common_deja_vu_recall_replay_executions": 1,
            "arms": [row.receipt() for row in self.arms],
            "cold_bootstrap_count": self.cold_bootstrap_count,
            "request_sequence": self.request_sequence,
            "full_current_rebuilds_this_request": self.full_current_rebuilds_this_request,
            "disk_json_sqlite_network_or_hash_calls_on_hot_path": 0,
            "authority": dict(AUTHORITY_FALSE),
        }


class HotCausalAblationEngine:
    """Main-held hot evaluator over one immutable memory/VRS generation."""

    def __init__(
        self,
        *,
        pair: FullCurrentMemoryVrsSnapshot,
        current_vrs: HotVrsStrengthIndex,
        frozen_vrs: HotVrsStrengthIndex,
        episode_roles: EpisodeRoleIndex,
    ) -> None:
        if not isinstance(current_vrs, HotVrsStrengthIndex) or not isinstance(
            frozen_vrs, HotVrsStrengthIndex
        ):
            raise TypeError(
                "VRS projection does not satisfy the hot strength-index contract"
            )
        if current_vrs.snapshot_id != pair.vrs_snapshot_id:
            raise ValueError(
                "current VRS projection is not bound to the full-current pair"
            )
        if frozen_vrs.snapshot_id == current_vrs.snapshot_id:
            raise ValueError("frozen VRS diagnostic must use a distinct projection")
        if pair.memory.lookup_requires_io:
            raise ValueError("full-current memory is not hot")
        self._pair = pair
        if current_vrs.lookup_requires_io or frozen_vrs.lookup_requires_io:
            raise ValueError("VRS strength index is not hot")
        self._current_vrs: HotVrsStrengthIndex = current_vrs
        self._frozen_vrs: HotVrsStrengthIndex = frozen_vrs
        self._episode_roles = episode_roles
        self._counter_lock = Lock()
        self._request_count = 0
        self._cold_bootstrap_count = 1

    @property
    def pair(self) -> FullCurrentMemoryVrsSnapshot:
        return self._pair

    @property
    def cold_bootstrap_count(self) -> int:
        return self._cold_bootstrap_count

    @property
    def request_count(self) -> int:
        with self._counter_lock:
            return self._request_count

    @staticmethod
    def _timed(function: Callable[[], Any]) -> tuple[Any, StageInterval]:
        started = time.perf_counter_ns()
        result = function()
        ended = time.perf_counter_ns()
        return result, StageInterval(started, ended)

    def _effective_strength(
        self,
        episode_id: str,
        *,
        projection: HotVrsStrengthIndex | None,
        promotion_roles: tuple[EpisodeRole, ...],
    ) -> float:
        if projection is None:
            return 0.0
        if self._episode_roles.role(episode_id) not in promotion_roles:
            return 0.0
        return projection.strength(episode_id)

    def evaluate(
        self,
        *,
        query: str,
        current_cues: Iterable[str],
        assess_current_evidence: EvidenceAssessor,
        decide: DecisionRule,
    ) -> CausalEvaluationReceipt:
        """Evaluate all arms without I/O, hashing, rebuilding, or state mutation."""

        with self._counter_lock:
            self._request_count += 1
            sequence = self._request_count

        signal, deja_vu_interval = self._timed(
            lambda: detect_deja_vu(
                self._pair.memory,
                query=query,
                current_cues=current_cues,
            )
        )
        recalled, recall_interval = self._timed(
            lambda: recall_memory(self._pair.memory, signal)
        )
        replayed, replay_interval = self._timed(
            lambda: replay_memory(self._pair.memory, recalled)
        )
        common = MappingProxyType(
            {
                "deja_vu": deja_vu_interval,
                "recall": recall_interval,
                "replay": replay_interval,
            }
        )
        definitions: tuple[
            tuple[
                ArmName,
                HotVrsStrengthIndex | None,
                tuple[EpisodeRole, ...],
            ],
            ...,
        ] = (
            (
                "full_current_current_vrs",
                self._current_vrs,
                ("base", "repair", "other"),
            ),
            (
                "full_current_frozen_vrs",
                self._frozen_vrs,
                ("base", "repair", "other"),
            ),
            ("full_current_no_vrs", None, ()),
            (
                "full_current_current_vrs_base_promotion_only",
                self._current_vrs,
                ("base",),
            ),
            (
                "full_current_current_vrs_repair_promotion_only",
                self._current_vrs,
                ("repair",),
            ),
        )
        arm_results = []
        for arm_name, projection, promotion_roles in definitions:

            def judge(
                episode: ReplayedEpisode,
                *,
                bound_projection: HotVrsStrengthIndex | None = projection,
                bound_roles: tuple[EpisodeRole, ...] = promotion_roles,
            ) -> CurrentEvidenceVerdict:
                strength = self._effective_strength(
                    episode.episode_id,
                    projection=bound_projection,
                    promotion_roles=bound_roles,
                )
                if strength < PROMOTION_THRESHOLD:
                    return CurrentEvidenceVerdict(
                        episode_id=episode.episode_id,
                        proposition=f"unpromoted:{episode.episode_id}",
                        verdict="insufficient",
                        rationale=(
                            "episode was recalled and replayed but the effective VRS "
                            "projection did not promote it"
                        ),
                        current_evidence_refs=(),
                    )
                return assess_current_evidence(episode)

            re_evidenced, re_evidence_interval = self._timed(
                lambda: re_evidence_memory(replayed, judge=judge)
            )
            activation = MemoryActivationReceipt(
                schema_version="swegca-memory-activation-v1",
                snapshot_id=self._pair.memory.snapshot_id,
                deja_vu=signal,
                recall=recalled,
                replay=replayed,
                re_evidence=re_evidenced,
            )
            if re_evidenced.should_abstain:
                decision = "abstain"
                invoked = False
            else:
                decision = str(decide(self._pair.memory, activation)).strip()
                if not decision:
                    raise ValueError("decision rule returned an empty proposal")
                invoked = True
            arm_results.append(
                CausalArmResult(
                    arm=arm_name,
                    effective_vrs_snapshot_id=(
                        None if projection is None else projection.snapshot_id
                    ),
                    promotion_roles=promotion_roles,
                    memory_activation=activation,
                    decision=decision,
                    decisive=decision != "abstain",
                    decision_rule_invoked=invoked,
                    re_evidence_interval=re_evidence_interval,
                )
            )
        return CausalEvaluationReceipt(
            pair_snapshot_id=self._pair.snapshot_id,
            memory_snapshot_id=self._pair.memory.snapshot_id,
            common_stage_intervals=common,
            arms=tuple(arm_results),
            cold_bootstrap_count=self._cold_bootstrap_count,
            request_sequence=sequence,
        )


class CompletedUnitCache:
    """Cold-loaded hot checkpoint view; persistence is an outer batch concern."""

    def __init__(
        self, completed: Mapping[str, Mapping[str, Any]] = MappingProxyType({})
    ) -> None:
        self._lock = Lock()
        self._completed = {key: dict(value) for key, value in completed.items()}

    def outcome(self, unit_id: str) -> Mapping[str, Any] | None:
        with self._lock:
            found = self._completed.get(unit_id)
            return None if found is None else MappingProxyType(dict(found))

    def store(self, unit_id: str, outcome: Mapping[str, Any]) -> Mapping[str, Any]:
        if not unit_id.strip() or not outcome:
            raise ValueError("completed outcome checkpoint is incomplete")
        with self._lock:
            if unit_id in self._completed and self._completed[unit_id] != dict(outcome):
                raise ValueError("completed checkpoint changed")
            self._completed[unit_id] = dict(outcome)
            return MappingProxyType(dict(self._completed[unit_id]))


@dataclass(frozen=True, slots=True)
class VisualOutcomeReceipt:
    unit_id: str
    outcome: Mapping[str, Any]
    checkpoint_hit: bool
    visual_worker_calls: int
    asr_worker_calls: int = 0
    ocr_worker_calls: int = 0

    def __post_init__(self) -> None:
        if (
            not self.unit_id.strip()
            or not self.outcome
            or self.asr_worker_calls
            or self.ocr_worker_calls
            or self.visual_worker_calls != (0 if self.checkpoint_hit else 1)
        ):
            raise ValueError("visual-only outcome contract changed")

    def receipt(self) -> dict[str, Any]:
        return {
            "unit_id": self.unit_id,
            "outcome": dict(self.outcome),
            "checkpoint_hit": self.checkpoint_hit,
            "visual_worker_calls": self.visual_worker_calls,
            "asr_worker_calls": self.asr_worker_calls,
            "ocr_worker_calls": self.ocr_worker_calls,
            "proposal_only": True,
            "authority": dict(AUTHORITY_FALSE),
        }


def acquire_visual_outcome_once(
    *,
    unit_id: str,
    visual_input: Any,
    checkpoint: CompletedUnitCache,
    visual_worker: VisualWorker,
) -> VisualOutcomeReceipt:
    """Use one visual proposal, or return an already completed hot checkpoint."""

    existing = checkpoint.outcome(unit_id)
    if existing is not None:
        return VisualOutcomeReceipt(unit_id, existing, True, 0)
    proposed = visual_worker(visual_input)
    if not isinstance(proposed, Mapping) or not proposed:
        raise TypeError("visual outcome worker returned an invalid proposal")
    stored = checkpoint.store(unit_id, proposed)
    return VisualOutcomeReceipt(unit_id, stored, False, 1)
