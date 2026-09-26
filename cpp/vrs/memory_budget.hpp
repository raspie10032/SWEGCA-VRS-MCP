#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <memory_resource>
#include <new>
#include <stdexcept>

namespace swegca::vrs {

// Main owns one shared budget for a VRS runtime. Accounting and limits stay
// outside SWEGCA's pure kernels. The caller selects the limit; there is no
// fixed 4GB ceiling in the architecture. This counts requested allocations,
// not OS RSS, allocator metadata, stacks, or file page cache.
// Like any memory_resource, it must outlive all objects allocated through it.
class MemoryBudget final : public std::pmr::memory_resource {
public:
    explicit MemoryBudget(std::size_t limit,
        std::pmr::memory_resource* upstream = std::pmr::new_delete_resource())
        : limit_(limit), upstream_(upstream) {
        if (!upstream_) throw std::invalid_argument("VRS memory upstream is null");
    }

    MemoryBudget(const MemoryBudget&) = delete;
    MemoryBudget& operator=(const MemoryBudget&) = delete;
    [[nodiscard]] std::size_t limit() const noexcept { return limit_; }
    [[nodiscard]] std::size_t used() const noexcept { return used_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::size_t peak_reserved() const noexcept { return peak_.load(std::memory_order_relaxed); }

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        // A zero-byte request still consumes one reservation unit, preventing
        // an unlimited number of zero-sized allocations under an empty budget.
        const auto charge = std::max<std::size_t>(bytes, 1);
        auto current = used_.load(std::memory_order_relaxed);
        do {
            if (charge > limit_ - current) throw std::bad_alloc();
        } while (!used_.compare_exchange_weak(current, current + charge,
                                              std::memory_order_relaxed));
        auto peak = peak_.load(std::memory_order_relaxed);
        while (peak < current + charge &&
               !peak_.compare_exchange_weak(peak, current + charge, std::memory_order_relaxed)) {}
        try {
            return upstream_->allocate(bytes, alignment);
        } catch (...) {
            used_.fetch_sub(charge, std::memory_order_relaxed);
            throw;
        }
    }

    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
        // Release the physical allocation before making its budget available.
        upstream_->deallocate(pointer, bytes, alignment);
        used_.fetch_sub(std::max<std::size_t>(bytes, 1), std::memory_order_relaxed);
    }

    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    const std::size_t limit_;
    std::pmr::memory_resource* const upstream_;
    std::atomic<std::size_t> used_{0};
    std::atomic<std::size_t> peak_{0};
};

}  // namespace swegca::vrs
