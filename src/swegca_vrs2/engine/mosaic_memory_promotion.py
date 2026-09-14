from __future__ import annotations
import math
from dataclasses import dataclass
VERIFIED_EXPERIENCE_PROMOTION_STRENGTH = 1.0


@dataclass(frozen=True)
class VRSExperiencePromotionDecision:
    snapshot_id: str
    connection_id: str
    previous_strength: float
    current_strength: float
    action: str
    promoted: bool
    semantic_evidence_allowed: bool
    underlying_experience_preserved: bool = True
    action_authorized: bool = False
    persistent_write_authorized: bool = False

    def __post_init__(self) -> None:
        if not self.snapshot_id.strip() or not self.connection_id.strip():
            raise ValueError("VRS promotion requires snapshot and connection identities")
        if any(
            not math.isfinite(value) or value < 0
            for value in (self.previous_strength, self.current_strength)
        ):
            raise ValueError("VRS promotion strengths must be finite and nonnegative")
        was_promoted = (
            self.previous_strength >= VERIFIED_EXPERIENCE_PROMOTION_STRENGTH
        )
        is_promoted = self.current_strength >= VERIFIED_EXPERIENCE_PROMOTION_STRENGTH
        expected_action = (
            "retain"
            if was_promoted and is_promoted
            else "revoke"
            if was_promoted
            else "promote"
            if is_promoted
            else "remain_unpromoted"
        )
        if (
            self.action != expected_action
            or self.promoted != is_promoted
            or self.semantic_evidence_allowed != is_promoted
        ):
            raise ValueError("VRS promotion decision changed")
        if (
            not self.underlying_experience_preserved
            or self.action_authorized
            or self.persistent_write_authorized
        ):
            raise ValueError("VRS promotion changed its authority boundary")


def assess_vrs_experience_promotion(
    *,
    snapshot_id: str,
    connection_id: str,
    previous_strength: float,
    current_strength: float,
) -> VRSExperiencePromotionDecision:
    """Derive semantic-evidence promotion from the current VRS snapshot only."""

    was_promoted = previous_strength >= VERIFIED_EXPERIENCE_PROMOTION_STRENGTH
    promoted = current_strength >= VERIFIED_EXPERIENCE_PROMOTION_STRENGTH
    action = (
        "retain"
        if was_promoted and promoted
        else "revoke"
        if was_promoted
        else "promote"
        if promoted
        else "remain_unpromoted"
    )
    return VRSExperiencePromotionDecision(
        snapshot_id=snapshot_id,
        connection_id=connection_id,
        previous_strength=previous_strength,
        current_strength=current_strength,
        action=action,
        promoted=promoted,
        semantic_evidence_allowed=promoted,
    )
