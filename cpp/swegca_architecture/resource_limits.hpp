#pragma once

#include <cstddef>
#include <cstdint>

namespace swegca::architecture {

// Product limits are one native contract. Subsystems may reserve less, but
// none may silently reinterpret these ceilings.
// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3f:638-640
struct ResourceLimits final {
    static constexpr std::uint64_t max_resident_bytes = 4'000'000'000ull;
    static constexpr std::uint64_t max_storage_bytes = 500'000'000'000ull;
    static constexpr std::uint64_t max_storage_bytes_per_second = 625'000'000ull;
    static constexpr std::size_t worker_count = 16;
};

}  // namespace swegca::architecture
