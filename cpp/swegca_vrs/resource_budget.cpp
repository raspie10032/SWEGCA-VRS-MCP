#include "swegca_vrs/resource_budget.hpp"

#include <new>
#include <stdexcept>

namespace swegca::vrs {

// SWEGCA: user@2026-09-22:89-92
RequestedMemoryBudget::RequestedMemoryBudget(std::uint64_t limit_bytes)
    : limit_(limit_bytes) {
    if (limit_bytes == 0) throw std::invalid_argument("vrs_memory_budget_invalid");
}

// The host counts requests before allocation. Failed physical allocations
// return the reservation; a configured budget refusal is distinguishable from
// physical OOM for cache eviction. This counts requested bytes, not RSS.
// SWEGCA: user@2026-09-22:89-92
void* RequestedMemoryBudget::allocate(std::size_t bytes, std::size_t alignment) {
    auto used = used_.load(std::memory_order_relaxed);
    do {
        if (bytes > limit_ - used) throw AllocationRefused{};
    } while (!used_.compare_exchange_weak(used, used + bytes,
                                          std::memory_order_acq_rel,
                                          std::memory_order_relaxed));
    try {
        if (alignment > __STDCPP_DEFAULT_NEW_ALIGNMENT__)
            return ::operator new(bytes, std::align_val_t(alignment));
        return ::operator new(bytes);
    } catch (...) {
        used_.fetch_sub(bytes, std::memory_order_acq_rel);
        throw;
    }
}

// SWEGCA: user@2026-09-22:89-92
void RequestedMemoryBudget::deallocate(void* pointer, std::size_t bytes,
                                       std::size_t alignment) noexcept {
    if (alignment > __STDCPP_DEFAULT_NEW_ALIGNMENT__)
        ::operator delete(pointer, std::align_val_t(alignment));
    else
        ::operator delete(pointer);
    used_.fetch_sub(bytes, std::memory_order_acq_rel);
}

}  // namespace swegca::vrs
