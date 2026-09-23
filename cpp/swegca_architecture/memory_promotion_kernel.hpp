#pragma once

#include "swegca_architecture/evidence_kernel.hpp"

#include <cstdint>

// SWEGCA nano-core: the pure decision that moves one memory candidate between
// the episodic and semantic tiers from one evidence judgment. Kernel rules as
// in evidence_kernel.hpp: no allocation, lock, exception, I/O or string; the
// kernel checks its own inputs and fails closed.
//
// Main-owned, not here (codex 2026-09-24 02:28, 02:31): that the judgment is
// an authentic decision of Main's registered accumulator; the provenance and
// counterfactual facts behind the two caller flags and their currency; the
// promotion authority the author attaches to an authoritative decision
// (mosaic_memory_promotion.py@3bddcb7:165-190); and applying the decision to
// the memory stores (:249-344).
//
// Rules: tinylm-slicer-sanabi-bazzite
// src/tinylm_slicer/mosaic_memory_promotion.py@3bddcb7:29-80,193-246
// (unchanged since 9aa5f555cf, 2026-08-25).
namespace swegca::architecture::kernel {

enum class MemoryTier : std::uint8_t {
    none = 0,
    episodic = 1,
    semantic = 2,
    quarantined = 3,
    retracted = 4,
};

enum class MemoryPromotionAction : std::uint8_t {
    quarantine = 1,
    retract = 2,
    promote = 3,
    refresh_semantic = 4,
    record_episode = 5,
};

// The author's reason is the judgment's own reason, or "incomplete_provenance".
// Values 1-8 equal EvidenceReason's, so the judgment's reason carries over
// unchanged; EvidenceReason::invalid_input (9) never reaches a decision.
enum class MemoryPromotionReason : std::uint8_t {
    minimum_effective_samples = 1,
    source_diversity = 2,
    axis_source_diversity = 3,
    context_diversity = 4,
    regime_change_suspected = 5,
    causal_lower_bound = 6,
    upper_bound_below_threshold = 7,
    uncertain = 8,
    incomplete_provenance = 10,
};

struct MemoryPromotionDecision {
    MemoryTier previous_tier = MemoryTier::none;
    MemoryTier next_tier = MemoryTier::none;
    MemoryPromotionAction action = MemoryPromotionAction::record_episode;
    MemoryPromotionReason reason = MemoryPromotionReason::uncertain;
    bool semantic_read_allowed = false;
};

// The linked World-memory transaction has its own minima, separate from the
// evidence policy that produced the decision. They are caller configuration;
// the core owns only the comparison, not the decision's authority.
// Lineage: direct — WorldMemoryTransactionConfig defaults and validation.
// SWEGCA: src/tinylm_slicer/mosaic_world_memory_transaction.py@3bddcb7:27-37
struct SemanticPromotionThresholds {
    double minimum_causal_lower_bound = 0.55;
    std::uint32_t minimum_source_diversity = 2;
    std::uint32_t minimum_context_diversity = 4;
};

// A typed unsigned diversity minimum cannot be negative; zero is invalid.
// Both comparisons reject NaN and infinities without needing math libraries.
// Lineage: direct — the source's post-init checks on this separate config.
// SWEGCA: src/tinylm_slicer/mosaic_world_memory_transaction.py@3bddcb7:27-37
[[nodiscard]] constexpr bool semantic_promotion_thresholds_valid(
    const SemanticPromotionThresholds& thresholds) noexcept {
    return thresholds.minimum_causal_lower_bound >= 0.0 &&
           thresholds.minimum_causal_lower_bound <= 1.0 &&
           thresholds.minimum_source_diversity > 0 &&
           thresholds.minimum_context_diversity > 0;
}

// This reproduces only the linked-promotion evidence threshold checks. Main
// must separately authenticate the EvidenceDecision, verify the current state
// against the write receipt, check candidate references, and own the native
// transaction and capability. It does not add a second judgment-validity rule.
// The source refuses a lower bound only when `value < minimum`; NaN would
// pass that one comparison. This `>=` fails closed for NaN. An authoritative
// accepted judgment cannot carry NaN: its accumulator requires a lower bound
// above its threshold, which is false for NaN. The difference is unreachable
// after Main authenticates that judgment.
// Lineage: direct — the author's accepted-status and three minima checks.
// SWEGCA: src/tinylm_slicer/mosaic_world_memory_transaction.py@3bddcb7:226-233
[[nodiscard]] constexpr bool semantic_promotion_evidence_eligible(
    const EvidenceJudgment& judgment,
    const SemanticPromotionThresholds& thresholds) noexcept {
    return semantic_promotion_thresholds_valid(thresholds) &&
           judgment.status == EvidenceStatus::accept &&
           judgment.causal_lower_bound >= thresholds.minimum_causal_lower_bound &&
           judgment.source_diversity >= thresholds.minimum_source_diversity &&
           judgment.context_diversity >= thresholds.minimum_context_diversity;
}

// Lineage: native mechanism — the author's tier and accumulator values are
// closed enumerations that never hold another value; C++ enums can, so the
// kernel admits only a tier the author defines and a status/reason pair the
// author's accumulator produces (accept only with causal_lower_bound, reject
// only with upper_bound_below_threshold, abstain with the other six).
// SWEGCA: src/tinylm_slicer/mosaic_evidence_accumulator.py@3bddcb7:333-354
[[nodiscard]] constexpr bool memory_promotion_input_valid(MemoryTier current,
                                                          EvidenceStatus status,
                                                          EvidenceReason reason) noexcept {
    if (static_cast<std::uint8_t>(current) > static_cast<std::uint8_t>(MemoryTier::retracted))
        return false;
    switch (status) {
    case EvidenceStatus::accept:
        return reason == EvidenceReason::causal_lower_bound;
    case EvidenceStatus::reject:
        return reason == EvidenceReason::upper_bound_below_threshold;
    case EvidenceStatus::abstain:
        switch (reason) {
        case EvidenceReason::minimum_effective_samples:
        case EvidenceReason::source_diversity:
        case EvidenceReason::axis_source_diversity:
        case EvidenceReason::context_diversity:
        case EvidenceReason::regime_change_suspected:
        case EvidenceReason::uncertain:
            return true;
        default:
            return false;
        }
    }
    return false;
}

// Lineage: direct — the author's five branches in the author's order:
// incomplete provenance quarantines; a rejection retracts; an acceptance with
// a verified counterfactual promotes (or refreshes a semantic memory) and
// alone allows semantic reads; a semantic memory under a suspected regime
// change or without a verified counterfactual is quarantined; anything else
// is recorded as an episode. An input the author cannot reach (see
// memory_promotion_input_valid) yields no decision: the function returns
// false and leaves `out` unchanged (codex 2026-09-24 02:31). The Main shell
// must fail closed on false: no promotion authority, no store change, and no
// stale `out` read as a decision (codex 02:35).
// SWEGCA: src/tinylm_slicer/mosaic_memory_promotion.py@3bddcb7:193-246
[[nodiscard]] constexpr bool decide_memory_promotion(MemoryTier current,
                                                     EvidenceStatus status,
                                                     EvidenceReason reason,
                                                     bool counterfactual_verified,
                                                     bool provenance_complete,
                                                     MemoryPromotionDecision& out) noexcept {
    if (!memory_promotion_input_valid(current, status, reason))
        return false;
    const auto carried = static_cast<MemoryPromotionReason>(reason);
    MemoryPromotionDecision decision;
    decision.previous_tier = current;
    if (!provenance_complete) {
        decision.next_tier = MemoryTier::quarantined;
        decision.action = MemoryPromotionAction::quarantine;
        decision.reason = MemoryPromotionReason::incomplete_provenance;
    } else if (status == EvidenceStatus::reject) {
        decision.next_tier = MemoryTier::retracted;
        decision.action = MemoryPromotionAction::retract;
        decision.reason = carried;
    } else if (status == EvidenceStatus::accept && counterfactual_verified) {
        decision.next_tier = MemoryTier::semantic;
        decision.action = current == MemoryTier::semantic
                              ? MemoryPromotionAction::refresh_semantic
                              : MemoryPromotionAction::promote;
        decision.reason = carried;
        decision.semantic_read_allowed = true;
    } else if (current == MemoryTier::semantic &&
               (reason == EvidenceReason::regime_change_suspected || !counterfactual_verified)) {
        decision.next_tier = MemoryTier::quarantined;
        decision.action = MemoryPromotionAction::quarantine;
        decision.reason = carried;
    } else {
        decision.next_tier = MemoryTier::episodic;
        decision.action = MemoryPromotionAction::record_episode;
        decision.reason = carried;
    }
    out = decision;
    return true;
}

}  // namespace swegca::architecture::kernel
