#include "swegca_architecture/evidence_kernel.hpp"
#include "swegca_architecture/evidence_rules.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>

#ifdef __FAST_MATH__
#error "SWEGCA evidence tests require fast math to be disabled"
#endif

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
    tally.revision = 400;
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
    // Fixed output bits from the author's Python formula for the four
    // baseline cases. These also catch a changed Wilson evaluation order.
    struct ExpectedBits {
        std::size_t scenario;
        std::uint64_t posterior, lower, upper, regime;
    };
    constexpr std::array expected_bits{
        ExpectedBits{0, 0x3fefeb9f34380a30ULL, 0x3fef28334f77f496ULL,
                     0x3fefffffffffffffULL, 0x0ULL},
        ExpectedBits{6, 0x3f6460cbc7f5cf9aULL, 0x0ULL,
                     0x3f7b84c51ef969ccULL, 0x0ULL},
        ExpectedBits{7, 0x3fe0000000000000ULL, 0x3fd13bbfe0617c6eULL,
                     0x3fe406f7be2b5adcULL, 0x0ULL},
        ExpectedBits{5, 0x3fefeb9f34380a30ULL, 0x3fef28334f77f496ULL,
                     0x3fefffffffffffffULL, 0x3fefeb9f34380a30ULL},
    };
    for (const auto& expected : expected_bits) {
        const auto value = sk::judge_evidence(rules, scenarios[expected.scenario].tally);
        if (std::bit_cast<std::uint64_t>(value.posterior_mean) != expected.posterior ||
            std::bit_cast<std::uint64_t>(value.causal_lower_bound) != expected.lower ||
            std::bit_cast<std::uint64_t>(value.overall_upper_bound) != expected.upper ||
            std::bit_cast<std::uint64_t>(value.regime_change_score) != expected.regime) {
            std::cerr << scenarios[expected.scenario].name
                      << ": numeric result differs from source bits\n";
            ++failures;
        }
    }
    const auto expect_invalid = [&](const char* label, sk::EvidenceTally tally) {
        const auto value = sk::judge_evidence(rules, tally);
        if (value.status != sk::EvidenceStatus::abstain ||
            value.reason != sk::EvidenceReason::invalid_input) {
            std::cerr << label << ": invalid tally was not rejected\n";
            ++failures;
        }
    };
    auto invalid_tally = supporting();
    invalid_tally.axis_support[0] = std::numeric_limits<double>::quiet_NaN();
    expect_invalid("nan_axis", invalid_tally);
    invalid_tally = supporting();
    invalid_tally.axis_refute[0] = std::numeric_limits<double>::infinity();
    expect_invalid("infinite_axis", invalid_tally);
    invalid_tally = supporting();
    invalid_tally.axis_support[0] = -1;
    expect_invalid("negative_axis", invalid_tally);
    invalid_tally = supporting();
    invalid_tally.recent_count = 7;
    expect_invalid("long_window", invalid_tally);
    invalid_tally = supporting();
    invalid_tally.recent_count = 4;
    invalid_tally.recent_sum = 5;
    expect_invalid("oversized_recent_sum", invalid_tally);
    invalid_tally = supporting();
    invalid_tally.recent_sum = std::numeric_limits<double>::quiet_NaN();
    expect_invalid("nan_recent_sum", invalid_tally);

    auto short_and_narrow = supporting();
    short_and_narrow.axis_support[0] = 3;
    short_and_narrow.source_diversity = 1;
    if (sk::judge_evidence(rules, short_and_narrow).reason !=
        sk::EvidenceReason::minimum_effective_samples) {
        std::cerr << "sample gate lost priority over source gate\n";
        ++failures;
    }
    auto context_and_regime = supporting();
    context_and_regime.context_diversity = 3;
    context_and_regime.recent_count = 4;
    if (sk::judge_evidence(rules, context_and_regime).reason !=
        sk::EvidenceReason::context_diversity) {
        std::cerr << "context gate lost priority over regime gate\n";
        ++failures;
    }
    auto at_sample_minimum = supporting();
    at_sample_minimum.axis_support.fill(4);
    if (sk::judge_evidence(rules, at_sample_minimum).reason ==
        sk::EvidenceReason::minimum_effective_samples) {
        std::cerr << "exact minimum samples were refused\n";
        ++failures;
    }
    auto at_accept = sa::EvidencePolicy{};
    at_accept.accept_margin = 0;
    at_accept.chance_rate = sk::judge_evidence(rules, supporting()).causal_lower_bound;
    const auto accept_boundary =
        sk::judge_evidence(sa::make_evidence_rules(at_accept), supporting());
    if (accept_boundary.status == sk::EvidenceStatus::accept) {
        std::cerr << "equal lower bound was accepted\n";
        ++failures;
    }
    auto at_reject = sa::EvidencePolicy{};
    at_reject.accept_margin = 0;
    at_reject.chance_rate = sk::judge_evidence(rules, scenarios[6].tally).overall_upper_bound;
    const auto reject_boundary =
        sk::judge_evidence(sa::make_evidence_rules(at_reject), scenarios[6].tally);
    if (reject_boundary.status != sk::EvidenceStatus::reject) {
        std::cerr << "equal upper bound was not rejected\n";
        ++failures;
    }
    auto at_regime = sa::EvidencePolicy{};
    at_regime.regime_change_threshold =
        sk::judge_evidence(rules, scenarios[5].tally).regime_change_score;
    const auto regime_boundary =
        sk::judge_evidence(sa::make_evidence_rules(at_regime), scenarios[5].tally);
    if (regime_boundary.reason != sk::EvidenceReason::regime_change_suspected) {
        std::cerr << "equal regime score was not suspected\n";
        ++failures;
    }

    // A confident producer supplies no evidence to this verifier. A fresh
    // revision with an empty tally must therefore never become accepted.
    sk::EvidenceTally empty;
    empty.revision = 1;
    if (sk::judge_evidence(rules, empty).status != sk::EvidenceStatus::abstain) {
        std::cerr << "empty evidence was accepted\n";
        ++failures;
    }

    const auto default_digest = sa::evidence_policy_digest(sa::EvidencePolicy{});
    auto changed_policy = sa::EvidencePolicy{};
    changed_policy.accept_margin += 0.01;
    if (sa::evidence_policy_digest(changed_policy) == default_digest) {
        std::cerr << "policy change left digest unchanged\n";
        ++failures;
    }
    auto invalid_policy = sa::EvidencePolicy{};
    invalid_policy.axis_count = 0;
    try {
        static_cast<void>(sa::make_evidence_rules(invalid_policy));
        std::cerr << "invalid policy was accepted\n";
        ++failures;
    } catch (const std::invalid_argument&) {
    }
    // Reference bits from the source author's NormalDist().inv_cdf on
    // CPython 3.14.7; cover the central and both tail branches of AS241.
    struct QuantileCase { double probability; std::uint64_t bits; };
    constexpr std::array quantiles{
        QuantileCase{1e-300, 0xc04286074064c26eULL},
        QuantileCase{1e-20, 0xc022865170b43a4bULL},
        QuantileCase{0.02, 0xc0006e13e8aadfdbULL},
        QuantileCase{0.075, 0xbff7085226d3e523ULL},
        QuantileCase{0.925, 0x3ff7085226d3e524ULL},
        QuantileCase{0.999999, 0x40130381a97985f1ULL},
    };
    for (const auto& vector : quantiles)
        if (std::bit_cast<std::uint64_t>(
                sa::standard_normal_quantile(vector.probability)) != vector.bits) {
            std::cerr << "normal quantile differs from source bits\n";
            ++failures;
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

    // Finite input values can still overflow an intermediate posterior sum.
    auto overflow = supporting();
    overflow.axis_support.fill(std::numeric_limits<double>::max() * 0.225);
    auto large_prior = sa::EvidencePolicy{};
    large_prior.prior_alpha = std::numeric_limits<double>::max() * 0.2;
    const auto overflow_verdict =
        sk::judge_evidence(sa::make_evidence_rules(large_prior), overflow);
    if (overflow_verdict.status != sk::EvidenceStatus::abstain ||
        overflow_verdict.reason != sk::EvidenceReason::invalid_input) {
        std::cerr << "derived nonfinite posterior did not fail closed\n";
        ++failures;
    }
    auto aggregate_overflow = supporting();
    aggregate_overflow.axis_support.fill(0);
    aggregate_overflow.axis_support[0] = 1e308;
    aggregate_overflow.axis_support[1] = 1e308;
    const auto aggregate_verdict = sk::judge_evidence(rules, aggregate_overflow);
    if (aggregate_verdict.status != sk::EvidenceStatus::abstain ||
        aggregate_verdict.reason != sk::EvidenceReason::invalid_input) {
        std::cerr << "nonfinite aggregate samples did not fail closed\n";
        ++failures;
    }

    // At a zero regime threshold, the source algorithm compares the
    // unmeasured score of zero against the threshold too.
    auto zero_regime = sa::EvidencePolicy{};
    zero_regime.regime_change_threshold = 0;
    const auto zero_verdict =
        sk::judge_evidence(sa::make_evidence_rules(zero_regime), supporting());
    if (zero_verdict.status != sk::EvidenceStatus::abstain ||
        zero_verdict.reason != sk::EvidenceReason::regime_change_suspected) {
        std::cerr << "zero regime threshold differs from source decision\n";
        ++failures;
    }

    // Aliased output columns can write into an item outside [first, last).
    std::array<double, count + 1> aliased{};
    aliased.fill(-1);
    const sk::EvidenceJudgmentColumns overlapping{
        statuses, reasons, std::span<double>(aliased).first(count),
        std::span<double>(aliased).subspan(1), upper, samples, regimes};
    if (sk::judge_evidence_batch(rules, input, overlapping, 0, 1) ||
        aliased != std::array<double, count + 1>{-1, -1, -1, -1, -1,
                                                -1, -1, -1, -1, -1}) {
        std::cerr << "overlapping output columns were accepted or mutated\n";
        ++failures;
    }

    const auto before_support = support;
    const sk::EvidenceJudgmentColumns input_alias{
        statuses, reasons, std::span<double>(support).first(count),
        lower, upper, samples, regimes};
    if (sk::judge_evidence_batch(rules, input, input_alias, 0, 1) ||
        support != before_support) {
        std::cerr << "output overlapping input was accepted or mutated\n";
        ++failures;
    }

    auto two_axis_policy = sa::EvidencePolicy{};
    two_axis_policy.axis_count = 2;
    const auto two_axis_rules = sa::make_evidence_rules(two_axis_policy);
    auto two_axis_tally = supporting();
    two_axis_tally.axis_support[2] = -1;  // beyond the configured axes
    if (sk::judge_evidence(two_axis_rules, two_axis_tally).status !=
        sk::EvidenceStatus::accept) {
        std::cerr << "two-axis scalar judgment used an inactive axis\n";
        ++failures;
    }
    std::array<double, 2> two_support{100, 100}, two_refute{0, 0};
    std::array<std::uint32_t, 2> two_axis_sources{1, 1};
    std::array<std::uint32_t, 1> two_sources{2}, two_contexts{4},
        two_recent_counts{0};
    std::array<double, 1> two_recent_sums{0};
    std::array<std::uint64_t, 1> two_revisions{400};
    const sk::EvidenceColumns two_input{
        1, two_support, two_refute, two_axis_sources, two_sources,
        two_contexts, two_recent_counts, two_recent_sums, two_revisions};
    std::array<sk::EvidenceStatus, 1> two_statuses{};
    std::array<sk::EvidenceReason, 1> two_reasons{};
    std::array<double, 1> two_means{}, two_lower{}, two_upper{},
        two_samples{}, two_regimes{};
    const sk::EvidenceJudgmentColumns two_output{
        two_statuses, two_reasons, two_means, two_lower,
        two_upper, two_samples, two_regimes};
    if (!sk::judge_evidence_batch(two_axis_rules, two_input, two_output, 0, 1) ||
        !same(sk::judge_evidence(two_axis_rules, supporting()), two_output, 0)) {
        std::cerr << "two-axis batch differed from scalar\n";
        ++failures;
    }
    return failures == 0 ? 0 : 1;
}
