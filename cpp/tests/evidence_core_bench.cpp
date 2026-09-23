#include "swegca_architecture/evidence_kernel.hpp"
#include "swegca_architecture/evidence_rules.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>

#ifdef __FAST_MATH__
#error "SWEGCA evidence benchmark requires fast math to be disabled"
#endif

int main() {
    namespace sa = swegca::architecture;
    namespace sk = swegca::architecture::kernel;
    const auto rules = sa::make_evidence_rules(sa::EvidencePolicy{});
    sk::EvidenceTally tally;
    tally.axis_support.fill(100.0);
    tally.axis_source_diversity.fill(1);
    tally.source_diversity = 2;
    tally.context_diversity = 4;
    tally.revision = 1;

    // The volatile read varies one axis so the optimizer must perform each
    // judgment. Its cost is included in the reported hot-loop measurement.
    volatile double input[16]{
        97, 98, 99, 100, 101, 102, 103, 104,
        105, 106, 107, 108, 109, 110, 111, 112};
    constexpr std::uint64_t iterations = 2'000'000;
    std::uint64_t checksum = 0;
    const auto start = std::chrono::steady_clock::now();
    for (std::uint64_t item = 0; item < iterations; ++item) {
        tally.axis_support[0] = input[item & 15];
        const auto value = sk::judge_evidence(rules, tally);
        checksum += static_cast<std::uint8_t>(value.status);
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const double nanoseconds =
        std::chrono::duration<double, std::nano>(elapsed).count() / iterations;
    std::cout << "nanoseconds_per_judgment=" << nanoseconds
              << " iterations=" << iterations << " checksum=" << checksum << '\n';
}
