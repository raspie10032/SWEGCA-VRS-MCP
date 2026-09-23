#include "swegca_architecture/evidence_kernel.hpp"
#include "swegca_architecture/evidence_rules.hpp"

#include <array>
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
    const auto scalar_elapsed = std::chrono::steady_clock::now() - start;
    const double scalar_ns =
        std::chrono::duration<double, std::nano>(scalar_elapsed).count() / iterations;

    constexpr std::size_t batch_size = 16;
    std::array<double, 4 * batch_size> support{}, refute{};
    support.fill(100);
    std::array<std::uint32_t, 4 * batch_size> axis_sources{};
    axis_sources.fill(1);
    std::array<std::uint32_t, batch_size> sources{}, contexts{}, recent_counts{};
    sources.fill(2);
    contexts.fill(4);
    std::array<double, batch_size> recent_sums{};
    std::array<std::uint64_t, batch_size> revisions{};
    revisions.fill(1);
    const sk::EvidenceColumns columns{batch_size, support, refute, axis_sources,
                                      sources, contexts, recent_counts,
                                      recent_sums, revisions};
    std::array<sk::EvidenceStatus, batch_size> statuses{};
    std::array<sk::EvidenceReason, batch_size> reasons{};
    std::array<double, batch_size> means{}, lower{}, upper{}, samples{}, regimes{};
    const sk::EvidenceJudgmentColumns results{statuses, reasons, means, lower,
                                               upper, samples, regimes};
    constexpr std::uint64_t batches = iterations / batch_size;
    const auto batch_start = std::chrono::steady_clock::now();
    for (std::uint64_t batch = 0; batch < batches; ++batch) {
        for (std::size_t item = 0; item < batch_size; ++item)
            support[item] = input[(batch + item) & 15];
        if (!sk::judge_evidence_batch(rules, columns, results, 0, batch_size)) {
            std::cerr << "valid batch refused\n";
            return 2;
        }
        checksum += static_cast<std::uint8_t>(statuses[batch & 15]);
    }
    const auto batch_elapsed = std::chrono::steady_clock::now() - batch_start;
    const double batch_ns =
        std::chrono::duration<double, std::nano>(batch_elapsed).count() /
        (batches * batch_size);
    std::cout << "scalar_ns_per_judgment=" << scalar_ns
              << " batch_ns_per_item=" << batch_ns
              << " items_per_path=" << iterations
              << " checksum=" << checksum << '\n';
}
