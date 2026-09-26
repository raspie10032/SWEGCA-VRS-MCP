#pragma once

#include "swegca_architecture/session_kernel.hpp"

namespace swegca::architecture::kernel {

// User's temporary-first rule. An unavailable tier is an error, never a miss.
enum class RecallScope { unavailable, temporary, main };
[[nodiscard]] constexpr RecallScope recall_scope(bool temporary_usable, bool temporary_found) noexcept {
    if (!temporary_usable) return RecallScope::unavailable;
    return temporary_found ? RecallScope::temporary : RecallScope::main;
}
[[nodiscard]] constexpr bool main_session_readable(SessionPhase phase, bool usable) noexcept {
    return usable && phase == SessionPhase::published;
}

}  // namespace swegca::architecture::kernel
