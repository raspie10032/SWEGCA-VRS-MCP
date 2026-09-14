from __future__ import annotations
import math
from dataclasses import dataclass, replace
from .mosaic_memory_activation import MemoryActivationReceipt
from .mosaic_memory_promotion import VRSExperiencePromotionDecision, assess_vrs_experience_promotion
VRS_STABLE_REINFORCEMENT_FACTOR = 1.01
VRS_UNSTABLE_WEAKENING_FACTOR = 0.995


@dataclass(frozen=True)
class VRSConnectionStateUpdate:
    episode_id: str
    connection_id: str
    proposition: str
    verdict: str
    previous_strength: float
    current_strength: float
    update_action: str
    promotion: VRSExperiencePromotionDecision
    underlying_experience_preserved: bool = True
    source_judgments: tuple[tuple[str, str, str], ...] = ()

    def __post_init__(self) -> None:
        if (
            not self.episode_id.strip()
            or not self.connection_id.strip()
            or not self.proposition.strip()
            or any(
                not math.isfinite(value) or value < 0
                for value in (self.previous_strength, self.current_strength)
            )
            or not self.underlying_experience_preserved
        ):
            raise ValueError("VRS connection state update changed")


@dataclass(frozen=True)
class VRSStateUpdateReceipt:
    snapshot_id: str
    updates: tuple[VRSConnectionStateUpdate, ...]
    stage_order: tuple[str, ...] = (
        "deja_vu",
        "recall",
        "replay",
        "re_evidence",
        "vrs_state_update",
    )
    detached_state_only: bool = True
    persistent_state_mutated: bool = False
    action_authorized: bool = False
    persistent_write_authorized: bool = False
    semantic_promotion_authorized: bool = False

    def __post_init__(self) -> None:
        if (
            not self.snapshot_id.strip()
            or self.stage_order
            != (
                "deja_vu",
                "recall",
                "replay",
                "re_evidence",
                "vrs_state_update",
            )
            or len({row.connection_id for row in self.updates}) != len(self.updates)
            or not self.detached_state_only
            or self.persistent_state_mutated
            or self.action_authorized
            or self.persistent_write_authorized
            or self.semantic_promotion_authorized
        ):
            raise ValueError("VRS state update authority or identity changed")


def plan_vrs_state_update(
    receipt: MemoryActivationReceipt,
) -> VRSStateUpdateReceipt:
    """Apply current verdicts to a detached edge-strength view only."""

    replayed = {row.episode_id: row for row in receipt.replay.episodes}
    conflicts = set(receipt.re_evidence.conflicting_propositions)
    updates = []
    for judgment in receipt.re_evidence.judgments:
        episode = replayed[judgment.episode_id]
        edge_step = next(
            (
                step
                for step in episode.steps
                if ("edge_id" in step.observation and "vrs_strength" in step.observation)
                or ("canonical_group_id" in step.observation
                    and "deweighted_vrs_strength" in step.observation)
            ),
            None,
        )
        if edge_step is None:
            continue
        observation = edge_step.observation
        canonical = "canonical_group_id" in observation
        previous = float(observation["deweighted_vrs_strength" if canonical else "vrs_strength"])
        if judgment.proposition in conflicts:
            current = previous
            action = "abstain_conflict"
        elif judgment.verdict == "support":
            current = previous * VRS_STABLE_REINFORCEMENT_FACTOR
            action = "reinforce"
        elif judgment.verdict == "refute":
            current = previous * VRS_UNSTABLE_WEAKENING_FACTOR
            action = "weaken"
        else:
            current = previous
            action = "preserve_unresolved"
        connection_id = (f"vrs-edge-group:{int(observation['canonical_group_id'])}"
                         if canonical else f"vrs-edge:{int(observation['edge_id'])}")
        updates.append(
            VRSConnectionStateUpdate(
                episode_id=episode.episode_id,
                connection_id=connection_id,
                proposition=judgment.proposition,
                verdict=judgment.verdict,
                previous_strength=previous,
                current_strength=current,
                update_action=action,
                promotion=assess_vrs_experience_promotion(
                    snapshot_id=receipt.snapshot_id,
                    connection_id=connection_id,
                    previous_strength=previous,
                    current_strength=current,
                ),
            )
        )
    # Canonical groups and their logical member aliases are one connection,
    # not independent opportunities to multiply the same strength. Retain all
    # replayed judgments in the proposal; opposing directions preserve strength.
    grouped = {}
    for row in updates:
        grouped.setdefault(row.connection_id, []).append(row)
    unique = []
    for connection, rows in grouped.items():
        first = rows[0]
        if any(row.previous_strength != first.previous_strength for row in rows):
            raise ValueError('connection aliases disagree on current snapshot strength')
        directions = {row.update_action for row in rows}
        conflict = 'abstain_conflict' in directions or {'reinforce', 'weaken'} <= directions
        selected = next((row for row in rows if row.update_action in ('reinforce', 'weaken')), first)
        current = first.previous_strength if conflict else selected.current_strength
        unique.append(replace(selected,
            current_strength=current,
            update_action='abstain_conflict' if conflict else selected.update_action,
            promotion=assess_vrs_experience_promotion(snapshot_id=receipt.snapshot_id,
                connection_id=connection, previous_strength=first.previous_strength, current_strength=current),
            source_judgments=tuple((row.episode_id, row.proposition, row.verdict) for row in rows)))
    return VRSStateUpdateReceipt(receipt.snapshot_id, tuple(unique))
