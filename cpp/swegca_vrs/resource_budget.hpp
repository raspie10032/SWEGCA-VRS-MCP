#pragma once

#include "swegca_architecture/allocation.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>

// The VRS host selects a resource profile for its workload. The baseline
// profile can be 4 GB; a larger workload may select a larger budget. SWEGCA
// receives only the abstract AllocationResource interface. Weak source
// analogy: the reviewed flow requires hard budgets; the 4 GB baseline and
// VRS-owned accounting came from the user's later direct clarification.
// SWEGCA: user@2026-09-22:89-92
namespace swegca::vrs {

class RequestedMemoryBudget final : public architecture::AllocationResource {
public:
    // SWEGCA: user@2026-09-22:89-92
    explicit RequestedMemoryBudget(std::uint64_t limit_bytes);

    // SWEGCA: user@2026-09-22:89-92
    [[nodiscard]] void* allocate(std::size_t bytes, std::size_t alignment) override;
    // SWEGCA: user@2026-09-22:89-92
    void deallocate(void* pointer, std::size_t bytes,
                    std::size_t alignment) noexcept override;

    // SWEGCA: user@2026-09-22:89-92
    [[nodiscard]] std::uint64_t used_requested_bytes() const noexcept {
        return used_.load(std::memory_order_relaxed);
    }
    // SWEGCA: user@2026-09-22:89-92
    [[nodiscard]] std::uint64_t limit_bytes() const noexcept { return limit_; }

private:
    std::uint64_t limit_;
    std::atomic<std::uint64_t> used_{0};
};

}  // namespace swegca::vrs
