#pragma once

#include "swegca_architecture/session_kernel.hpp"
#include <cstddef>

namespace swegca::architecture::kernel {

// User's temporary-first rule. An unavailable tier is an error, never a miss.
enum class RecallScope { unavailable, temporary, main };
// A current exact cue precedes the continued memory key within one tier.
// Both are familiarity cues only, not evidence of current truth.
enum class FamiliarityKey { missing, exact, continuation };
// Receipt representation only. No candidate or evidence is discarded. Compare
// bounded snapshot metadata with individual address pins without multiplication
// overflow; the caller accounts for its own VRS storage representation.
[[nodiscard]] constexpr bool recall_range_receipt(std::size_t count,std::size_t range_bytes,
    std::size_t pin_bytes) noexcept {
    return pin_bytes && count >= range_bytes/pin_bytes + (range_bytes%pin_bytes!=0);
}
[[nodiscard]] constexpr FamiliarityKey familiarity_key(bool exact, bool continued) noexcept {
    return exact ? FamiliarityKey::exact : continued ? FamiliarityKey::continuation : FamiliarityKey::missing;
}
[[nodiscard]] constexpr RecallScope recall_scope(bool temporary_usable, bool temporary_found) noexcept {
    if (!temporary_usable) return RecallScope::unavailable;
    return temporary_found ? RecallScope::temporary : RecallScope::main;
}
[[nodiscard]] constexpr bool main_session_readable(SessionPhase phase, bool usable) noexcept {
    return usable && phase == SessionPhase::published;
}

}  // namespace swegca::architecture::kernel
