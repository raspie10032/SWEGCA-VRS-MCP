#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace swegca::architecture::kernel {

// One canonical connection is evaluated once for a pinned VRS generation.
// The shell first maps every judgment to its original row action, using the
// whole receipt's conflicting-proposition set, then collects all rows for
// this connection, including aliases. These flags are neither provenance
// nor publication authority. The shell must retain every judgment/address,
// require equal previous strengths under numeric equality (+0 == -0), and
// pass the first row's previous value here to preserve its stored zero sign.
struct ConnectionDirections final {
    bool has_judgment = false;
    bool reinforce = false;
    bool weaken = false;
    bool abstain_conflict = false;
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

// The source first builds and validates every alias row. Even if grouping
// later preserves strength for a conflict, a reinforce overflow must fail.
// A grouped connection then gets at most one 1.01 or 0.995 factor. An
// abstain_conflict action or both directions preserve it. Unresolved rows do
// not cancel an unopposed direction. This detached result grants no semantic
// write or action authority. The host maps only exact support/refute strings
// to direction, chooses the first eligible edge step and strength field,
// groups by the exact vrs-edge:/vrs-edge-group: connection id, retains the
// first reinforce/weaken row as representative (else first row), rejects
// duplicate output connection ids, binds stage order and false authority
// flags, and discards the whole proposal if any connection fails.
// SWEGCA: src/tinylm_slicer/mosaic_vrs_state_update.py@3bddcb7:63-161
// SWEGCA: src/tinylm_slicer/mosaic_memory_promotion.py@3bddcb7:83-153
[[nodiscard]] inline StrengthTransition update_vrs_strength(
    double previous, ConnectionDirections directions) noexcept {
    StrengthTransition out;
    if (!std::isfinite(previous) || previous < 0 || !directions.has_judgment)
        return out;

    // The original constructs candidate rows before grouping them. A
    // nonfinite candidate invalidates the entire receipt even when the
    // grouped connection would later abstain on conflicting directions.
    const double reinforced = directions.reinforce ? previous * 1.01 : previous;
    const double weakened = directions.weaken ? previous * 0.995 : previous;
    if (!std::isfinite(reinforced) || !std::isfinite(weakened)) return out;

    double current = previous;
    StrengthAction action = StrengthAction::preserve_unresolved;
    if (directions.abstain_conflict || (directions.reinforce && directions.weaken)) {
        action = StrengthAction::abstain_conflict;
    } else if (directions.reinforce) {
        current = reinforced;
        action = StrengthAction::reinforce;
    } else if (directions.weaken) {
        current = weakened;
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

// Native C++ batch integrity check; the source has no byte-range interface.
// Weak source analogy: the source validates a whole receipt before returning.
// SWEGCA: src/tinylm_slicer/mosaic_vrs_state_update.py@3bddcb7:63-161
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

// Native C++ byte-range helper; no Python counterpart.
// SWEGCA: src/tinylm_slicer/mosaic_vrs_state_update.py@3bddcb7:63-161
[[nodiscard]] inline bool strength_ranges_overlap(StrengthByteRange a,
                                                  StrengthByteRange b) noexcept {
    return a.begin < a.end && b.begin < b.end &&
           a.begin < b.end && b.begin < a.end;
}

// A caller can split [0, count) into disjoint ranges for its own scheduler.
// Every item uses the same scalar rule; an invalid item fails the whole range
// before outputs are written. The host must discard all ranges if any range
// fails; range-local success never constitutes a partially accepted receipt.
// Inputs and outputs must not overlap. The host owns allocation and thread
// count; the core has no global resource cap or mutable state. This range API
// is native C++ infrastructure, not an algorithm in the original source.
// SWEGCA: src/tinylm_slicer/mosaic_vrs_state_update.py@3bddcb7:63-161
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
