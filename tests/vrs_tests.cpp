#include "swegca_architecture/evidence_rules.hpp"
#include "vrs/verification.hpp"

#include <cstdio>
#include <cstdlib>
#include <limits>
#include <type_traits>

using namespace swegca::architecture;
using namespace swegca::architecture::kernel;
using namespace swegca::vrs;

static unsigned checks = 0;
#define CHECK(expression) do { \
    ++checks; \
    if (!(expression)) { \
        std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expression); \
        std::abort(); \
    } \
} while (false)

EvidenceTally tally(double supports, double refutes) {
    EvidenceTally result;
    result.axis_support.fill(supports);
    result.axis_refute.fill(refutes);
    result.axis_source_diversity.fill(2);
    result.source_diversity = 4;
    result.context_diversity = 6;
    result.revision = 1;
    return result;
}

int main() {
    static_assert(!std::is_aggregate_v<VerificationResult>);
    static_assert(std::is_trivially_copyable_v<VerificationResult>);
    static_assert(VerificationResult{}.connection_change() == ConnectionChange::preserve);
    const auto rules = make_evidence_rules(EvidencePolicy{});
    const auto accepted = verify_experience(rules, tally(100, 0));
    const auto rejected = verify_experience(rules, tally(0, 100));
    const auto uncertain = verify_experience(rules, tally(50, 50));
    EvidenceTally malformed; malformed.source_diversity = 1;
    const auto invalid = verify_experience(rules, malformed);
    const auto no_observations = verify_connection(rules, {}, 0.75);
    CHECK(no_observations.verification().judgment().reason() == EvidenceReason::minimum_effective_samples);
    CHECK(no_observations.strength().valid() && no_observations.strength().current() == 0.75);
    CHECK(accepted.judgment().status() == EvidenceStatus::accept);
    CHECK(accepted.connection_change() == ConnectionChange::strengthen);
    CHECK(rejected.judgment().status() == EvidenceStatus::reject);
    CHECK(rejected.connection_change() == ConnectionChange::weaken);
    CHECK(uncertain.judgment().reason() == EvidenceReason::uncertain);
    CHECK(uncertain.connection_change() == ConnectionChange::preserve);
    CHECK(invalid.judgment().reason() == EvidenceReason::invalid_input);
    CHECK(invalid.connection_change() == ConnectionChange::preserve);
    auto copied = accepted;
    copied = rejected;
    CHECK(copied.judgment().status() == EvidenceStatus::reject);
    CHECK(copied.connection_change() == ConnectionChange::weaken);
    copied = {};
    CHECK(copied.judgment().reason() == EvidenceReason::invalid_input);
    CHECK(copied.connection_change() == ConnectionChange::preserve);

    static_assert(!std::is_aggregate_v<ConnectionVerification>);
    static_assert(!std::is_aggregate_v<ConnectionStrengthResult>);
    const auto stronger = verify_connection(rules, tally(100, 0), 0.999);
    CHECK(stronger.strength().valid());
    CHECK(stronger.strength().previous() == 0.999);
    CHECK(stronger.strength().current() == 0.999 * 1.01);
    CHECK(stronger.strength().evidence_eligible());
    const auto weaker = verify_connection(rules, tally(0, 100), 1.0);
    CHECK(weaker.strength().valid());
    CHECK(weaker.strength().current() == 0.995);
    CHECK(!weaker.strength().evidence_eligible());
    const auto held = verify_connection(rules, tally(50, 50), 1.0);
    CHECK(held.strength().valid());
    CHECK(held.strength().current() == 1.0);
    CHECK(held.strength().evidence_eligible());
    const auto invalid_strength = verify_connection(rules, malformed, 1.0);
    CHECK(!invalid_strength.strength().valid());
    CHECK(!invalid_strength.strength().evidence_eligible());
    for (const auto previous : {-1.0, std::numeric_limits<double>::infinity(),
                                std::numeric_limits<double>::quiet_NaN(),
                                std::numeric_limits<double>::max()}) {
        const auto failed = verify_connection(rules, tally(100, 0), previous);
        CHECK(!failed.strength().valid());
        CHECK(!failed.strength().evidence_eligible());
    }
    const auto zero_strength = verify_connection(rules, tally(100, 0), 0.0);
    CHECK(zero_strength.strength().valid());
    CHECK(zero_strength.strength().current() == 0.0);
    auto overwritten = stronger;
    overwritten = invalid_strength;
    CHECK(!overwritten.strength().valid());
    CHECK(!overwritten.strength().evidence_eligible());
    std::printf("PASS: %u VRS checks\n", checks);
}
