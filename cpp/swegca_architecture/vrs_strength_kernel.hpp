#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace swegca::architecture::kernel {

// One canonical connection is evaluated once for a pinned VRS generation.
// The shell collects all replayed judgments for that connection, including
// aliases, before calling this pure kernel. It must retain those judgments
// and their experience addresses in the proposal; these booleans alone are
// not provenance or publication authority.
struct ConnectionDirections final {
    bool has_judgment = false;
    bool support = false;
    bool refute = false;
    bool conflicting_proposition = false;
};

enum class StrengthAction : std::uint8_t {
    invalid = 0,
    preserve_unresolved = 1,
    reinforce = 2,
    weaken = 3,
    abstain_conflict = 4,
};

enum class PromotionAction : std::uint8_t {
    none = 0,
    remain_unpromoted = 1,
    promote = 2,
    retain = 3,
    revoke = 4,
};

struct StrengthTransition final {
    double current = 0;
    StrengthAction action = StrengthAction::invalid;
    PromotionAction promotion = PromotionAction::none;
    bool valid = false;
};

// The original grouped update applies at most one 1.01 or 0.995 factor to
// each canonical connection. A conflicting proposition, or both directions
// among its aliases, preserves the old strength. An unresolved judgment does
// not cancel an otherwise unopposed support or refute. This result is a
// detached proposal only; it grants no semantic write or action authority.
// SWEGCA: src/tinylm_slicer/mosaic_vrs_state_update.py@3bddcb7:84-157
// SWEGCA: src/tinylm_slicer/mosaic_memory_promotion.py@3bddcb7:88-150
[[nodiscard]] inline StrengthTransition update_vrs_strength(
    double previous, ConnectionDirections directions) noexcept {
    StrengthTransition out;
    if (!std::isfinite(previous) || previous < 0 || !directions.has_judgment)
        return out;

    double current = previous;
    StrengthAction action = StrengthAction::preserve_unresolved;
    if (directions.conflicting_proposition || (directions.support && directions.refute)) {
        action = StrengthAction::abstain_conflict;
    } else if (directions.support) {
        current = previous * 1.01;
        action = StrengthAction::reinforce;
    } else if (directions.refute) {
        current = previous * 0.995;
        action = StrengthAction::weaken;
    }
    if (!std::isfinite(current) || current < 0) return out;

    const bool was_promoted = previous >= 1.0;
    const bool is_promoted = current >= 1.0;
    const auto promotion =
        was_promoted ? (is_promoted ? PromotionAction::retain : PromotionAction::revoke)
                     : (is_promoted ? PromotionAction::promote
                                    : PromotionAction::remain_unpromoted);
    return {current, action, promotion, true};
}

struct StrengthByteRange final {
    std::uintptr_t begin = 0;
    std::uintptr_t end = 0;
};

// Native batch integrity check: an output cannot alias either input.
// SWEGCA: src/tinylm_slicer/mosaic_vrs_state_update.py@3bddcb7:84-157
template <class T>
[[nodiscard]] inline bool strength_byte_range(std::span<T> values,
                                              StrengthByteRange& range) noexcept {
    if (values.empty()) {
        range = {};
        return true;
    }
    constexpr auto maximum = std::numeric_limits<std::uintptr_t>::max();
    if (values.size() > maximum / sizeof(T)) return false;
    const auto bytes = values.size() * sizeof(T);
    const auto begin = reinterpret_cast<std::uintptr_t>(values.data());
    if (begin > maximum - bytes) return false;
    range = {begin, begin + bytes};
    return true;
}

// SWEGCA: src/tinylm_slicer/mosaic_vrs_state_update.py@3bddcb7:84-157
[[nodiscard]] inline bool strength_ranges_overlap(StrengthByteRange a,
                                                  StrengthByteRange b) noexcept {
    return a.begin < a.end && b.begin < b.end &&
           a.begin < b.end && b.begin < a.end;
}

// A caller can split [0, count) into disjoint ranges for its own scheduler.
// Every item uses the same scalar rule; an invalid item fails the whole range
// before outputs are written, so a partially accepted batch is not exposed.
// Inputs and outputs must not overlap. The host owns allocation and thread
// count; the core has no global resource cap or mutable state.
// SWEGCA: src/tinylm_slicer/mosaic_vrs_state_update.py@3bddcb7:84-157
[[nodiscard]] inline bool update_vrs_strength_batch(
    std::span<const double> previous,
    std::span<const ConnectionDirections> directions,
    std::span<StrengthTransition> output,
    std::size_t first, std::size_t last) noexcept {
    if (directions.size() != previous.size() || output.size() != previous.size() ||
        first > last || last > previous.size())
        return false;
    StrengthByteRange strengths, judgments, results;
    if (!strength_byte_range(previous, strengths) ||
        !strength_byte_range(directions, judgments) ||
        !strength_byte_range(output, results) ||
        strength_ranges_overlap(results, strengths) ||
        strength_ranges_overlap(results, judgments))
        return false;
    for (std::size_t item = first; item < last; ++item)
        if (!update_vrs_strength(previous[item], directions[item]).valid)
            return false;
    for (std::size_t item = first; item < last; ++item)
        output[item] = update_vrs_strength(previous[item], directions[item]);
    return true;
}

}  // namespace swegca::architecture::kernel
