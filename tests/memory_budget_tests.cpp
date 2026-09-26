#include "vrs/memory_budget.hpp"

#include <atomic>
#include <barrier>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory_resource>
#include <thread>
#include <vector>

using swegca::vrs::MemoryBudget;
static unsigned checks = 0;
#define CHECK(expression) do { \
    ++checks; \
    if (!(expression)) { \
        std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expression); \
        std::abort(); \
    } \
} while (false)

template<class Function>
void expect_bad_alloc(Function operation) {
    bool failed = false;
    try { operation(); } catch (const std::bad_alloc&) { failed = true; }
    CHECK(failed);
}

class FailingUpstream final : public std::pmr::memory_resource {
    void* do_allocate(std::size_t, std::size_t) override { throw std::bad_alloc(); }
    void do_deallocate(void*, std::size_t, std::size_t) override { std::abort(); }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override { return this == &other; }
};

int main() {
    MemoryBudget memory(32);
    auto first = memory.allocate(24);
    CHECK(memory.used() == 24);
    expect_bad_alloc([&] { (void)memory.allocate(9); });
    CHECK(memory.used() == 24);
    auto second = memory.allocate(8);
    CHECK(memory.used() == 32);
    CHECK(memory.peak_reserved() == 32);
    expect_bad_alloc([&] { (void)memory.allocate(1); });
    expect_bad_alloc([&] { (void)memory.allocate(std::numeric_limits<std::size_t>::max()); });
    memory.deallocate(first, 24);
    CHECK(memory.used() == 8);
    memory.deallocate(second, 8);
    CHECK(memory.used() == 0);
    CHECK(memory.peak_reserved() == 32);
    {
        std::pmr::vector<std::byte> values(&memory);
        values.resize(24);
        CHECK(memory.used() == values.capacity());
        expect_bad_alloc([&] { values.resize(40); });
        CHECK(values.size() == 24);
        CHECK(memory.used() == values.capacity());
    }
    CHECK(memory.used() == 0);
    MemoryBudget empty(0);
    expect_bad_alloc([&] { (void)empty.allocate(0); });
    CHECK(empty.used() == 0);
    auto zero = memory.allocate(0);
    CHECK(memory.used() == 1);
    memory.deallocate(zero, 0);
    CHECK(memory.used() == 0);
    FailingUpstream failing;
    MemoryBudget with_failure(128, &failing);
    expect_bad_alloc([&] { (void)with_failure.allocate(100); });
    CHECK(with_failure.used() == 0);
    CHECK(with_failure.peak_reserved() == 100);
    MemoryBudget aligned(512);
    auto pointer = aligned.allocate(128, 256);
    CHECK(reinterpret_cast<std::uintptr_t>(pointer) % 256 == 0);
    CHECK(aligned.used() == 128);
    aligned.deallocate(pointer, 128, 256);
    CHECK(aligned.used() == 0);
    // More than 4GB is a permitted configuration; this test allocates 1 byte,
    // not a multi-gigabyte workload or an RSS compliance experiment.
    MemoryBudget larger(std::size_t{8} * 1024 * 1024 * 1024);
    auto tiny = larger.allocate(1);
    CHECK(larger.limit() == std::size_t{8} * 1024 * 1024 * 1024);
    larger.deallocate(tiny, 1);

    MemoryBudget shared(144);
    std::barrier phase(2);
    std::atomic<unsigned> succeeded{0}, refused{0}, invalid{0};
    constexpr unsigned rounds = 2000;
    const auto worker = [&] {
        for (unsigned round = 0; round < rounds; ++round) {
            phase.arrive_and_wait();
            void* allocated = nullptr;
            try {
                allocated = shared.allocate(96);
                succeeded.fetch_add(1, std::memory_order_relaxed);
            } catch (const std::bad_alloc&) {
                refused.fetch_add(1, std::memory_order_relaxed);
            }
            // Both allocation attempts finish before the winner releases it.
            phase.arrive_and_wait();
            if (shared.used() != 96) invalid.fetch_add(1, std::memory_order_relaxed);
            phase.arrive_and_wait();
            if (allocated) shared.deallocate(allocated, 96);
            phase.arrive_and_wait();
        }
    };
    {
        std::jthread left(worker), right(worker);
    }
    CHECK(succeeded == rounds);
    CHECK(refused == rounds);
    CHECK(invalid == 0);
    CHECK(shared.used() == 0);
    CHECK(shared.peak_reserved() == 96);
    std::printf("PASS: %u memory budget checks; %u contended reservation rounds\n", checks, rounds);
}
