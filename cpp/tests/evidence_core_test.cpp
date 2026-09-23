#include "swegca_architecture/evidence_kernel.hpp"
#include "swegca_architecture/evidence_rules.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>

namespace sa = swegca::architecture;
namespace sk = swegca::architecture::kernel;

namespace {

struct Case {
    const char* name;
    sk::EvidenceTally tally;
    sk::EvidenceStatus status;
    sk::EvidenceReason reason;
};

sk::EvidenceTally supporting() {
    sk::EvidenceTally tally;
    tally.axis_support.fill(100.0);
    tally.axis_source_diversity.fill(1);
    tally.source_diversity = 2;
    tally.context_diversity = 4;
    tally.revision = 1;
    return tally;
}

std::array<Case, 9> cases() {
    auto accepted = supporting();
    auto short_axis = accepted;
    short_axis.axis_support[0] = 3.0;
    auto one_source = accepted;
    one_source.source_diversity = 1;
    auto one_axis_source = accepted;
    one_axis_source.axis_source_diversity[0] = 0;
    auto few_contexts = accepted;
    few_contexts.context_diversity = 3;
    auto regime_change = accepted;
    regime_change.recent_count = 4;
    regime_change.recent_sum = 0.0;
    auto rejected = accepted;
    rejected.axis_support.fill(0.0);
    rejected.axis_refute.fill(100.0);
    auto uncertain = accepted;
    uncertain.axis_support.fill(5.0);
    uncertain.axis_refute.fill(5.0);
    auto invalid = accepted;
    invalid.revision = 0;
    return {{{"support", accepted, sk::EvidenceStatus::accept,
              sk::EvidenceReason::causal_lower_bound},
             {"short_axis", short_axis, sk::EvidenceStatus::abstain,
              sk::EvidenceReason::minimum_effective_samples},
             {"source_diversity", one_source, sk::EvidenceStatus::abstain,
              sk::EvidenceReason::source_diversity},
             {"axis_source_diversity", one_axis_source, sk::EvidenceStatus::abstain,
              sk::EvidenceReason::axis_source_diversity},
             {"context_diversity", few_contexts, sk::EvidenceStatus::abstain,
              sk::EvidenceReason::context_diversity},
             {"regime_change", regime_change, sk::EvidenceStatus::abstain,
              sk::EvidenceReason::regime_change_suspected},
             {"refutation", rejected, sk::EvidenceStatus::reject,
              sk::EvidenceReason::upper_bound_below_threshold},
             {"uncertain", uncertain, sk::EvidenceStatus::abstain,
              sk::EvidenceReason::uncertain},
             {"invalid", invalid, sk::EvidenceStatus::abstain,
              sk::EvidenceReason::invalid_input}}};
}

bool same(const sk::EvidenceJudgment& expected,
          const sk::EvidenceJudgmentColumns& batch, std::size_t at) {
    return expected.status == batch.status[at] &&
           expected.reason == batch.reason[at] &&
           expected.posterior_mean == batch.posterior_mean[at] &&
           expected.causal_lower_bound == batch.causal_lower_bound[at] &&
           expected.overall_upper_bound == batch.overall_upper_bound[at] &&
           expected.effective_sample_size == batch.effective_sample_size[at] &&
           expected.regime_change_score == batch.regime_change_score[at];
}

}  // namespace

int main() {
    const auto rules = sa::make_evidence_rules(sa::EvidencePolicy{});
    const auto scenarios = cases();
    constexpr std::size_t count = 9;
    constexpr std::size_t axes = 4;
    int failures = 0;
    for (const auto& scenario : scenarios) {
        const auto verdict = sk::judge_evidence(rules, scenario.tally);
        if (verdict.status != scenario.status || verdict.reason != scenario.reason) {
            std::cerr << scenario.name << ": incorrect three-state decision\n";
            ++failures;
        }
    }

    std::array<double, axes * count> support{};
    std::array<double, axes * count> refute{};
    std::array<std::uint32_t, axes * count> axis_sources{};
    std::array<std::uint32_t, count> sources{};
    std::array<std::uint32_t, count> contexts{};
    std::array<std::uint32_t, count> recent_counts{};
    std::array<double, count> recent_sums{};
    std::array<std::uint64_t, count> revisions{};
    for (std::size_t item = 0; item < count; ++item) {
        const auto& tally = scenarios[item].tally;
        for (std::size_t axis = 0; axis < axes; ++axis) {
            support[axis * count + item] = tally.axis_support[axis];
            refute[axis * count + item] = tally.axis_refute[axis];
            axis_sources[axis * count + item] = tally.axis_source_diversity[axis];
        }
        sources[item] = tally.source_diversity;
        contexts[item] = tally.context_diversity;
        recent_counts[item] = tally.recent_count;
        recent_sums[item] = tally.recent_sum;
        revisions[item] = tally.revision;
    }
    const sk::EvidenceColumns input{count, support, refute, axis_sources,
                                     sources, contexts, recent_counts, recent_sums, revisions};
    std::array<sk::EvidenceStatus, count> statuses{};
    std::array<sk::EvidenceReason, count> reasons{};
    std::array<double, count> means{}, lower{}, upper{}, samples{}, regimes{};
    const sk::EvidenceJudgmentColumns output{statuses, reasons, means, lower, upper, samples,
                                              regimes};
    if (!sk::judge_evidence_batch(rules, input, output, 0, 4) ||
        !sk::judge_evidence_batch(rules, input, output, 4, count)) {
        std::cerr << "valid split batch was refused\n";
        ++failures;
    } else {
        for (std::size_t item = 0; item < count; ++item)
            if (!same(sk::judge_evidence(rules, scenarios[item].tally), output, item)) {
                std::cerr << scenarios[item].name << ": split batch differs from scalar\n";
                ++failures;
            }
    }

    auto bad = input;
    bad.axis_support = std::span<const double>(support).first(support.size() - 1);
    const auto before_status = statuses;
    const auto before_reason = reasons;
    const auto before_means = means;
    const auto before_lower = lower;
    const auto before_upper = upper;
    const auto before_samples = samples;
    const auto before_regimes = regimes;
    if (sk::judge_evidence_batch(rules, bad, output, 0, count) ||
        statuses != before_status || reasons != before_reason || means != before_means ||
        lower != before_lower || upper != before_upper || samples != before_samples ||
        regimes != before_regimes) {
        std::cerr << "invalid batch shape changed output\n";
        ++failures;
    }
    return failures == 0 ? 0 : 1;
}
