#include "world/evidence_accumulator.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace swegca::world;

void check(const bool condition, const char* expression, const int line) {
    if (!condition) {
        std::cerr << "CHECK failed at line " << line << ": " << expression << '\n';
        std::exit(1);
    }
}

#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

EvidenceObservation observation(
    const int index, std::string outcome = "support",
    std::string hypothesis = "rule-a", std::string axis = {},
    std::string source = {}, std::string context = {},
    std::string address = {}, std::string producer = {},
    const double confidence = 0.5,
    std::optional<std::int64_t> expires_at = std::nullopt,
    std::string source_address = {}, std::string source_revision = {}) {
    static const EvidenceAccumulatorConfig config;
    return EvidenceObservation(
        std::move(hypothesis),
        address.empty() ? "evidence:" + std::to_string(index) : std::move(address),
        source.empty() ? "source:" + std::to_string(index % 2) : std::move(source),
        context.empty() ? "context:" + std::to_string(index) : std::move(context),
        axis.empty() ? config.required_axes[static_cast<std::size_t>(index) %
                                            config.required_axes.size()]
                     : std::move(axis),
        std::move(outcome), index, expires_at,
        producer.empty() ? "producer:" + std::to_string(index) : std::move(producer),
        confidence, std::move(source_address), std::move(source_revision));
}

std::shared_ptr<const EvidenceAccumulatorState> feed(
    std::shared_ptr<const EvidenceAccumulatorState> state,
    const std::vector<EvidenceObservation>& observations,
    const EvidenceAccumulatorConfig& config = EvidenceAccumulatorConfig()) {
    for (const auto& item : observations)
        state = update_accumulator(state, item, config, item.observed_at).state;
    return state;
}

std::shared_ptr<const EvidenceAccumulatorState> accepted_state() {
    const EvidenceAccumulatorConfig config;
    auto state = EvidenceAccumulatorState::empty("rule-a", config);
    std::vector<EvidenceObservation> observations;
    for (int index = 0; index != 16; ++index) observations.push_back(observation(index));
    return feed(std::move(state), observations, config);
}

void check_fifteen_abstain_sixteen_accept() {
    const EvidenceAccumulatorConfig config;
    auto state = EvidenceAccumulatorState::empty("rule-a", config);
    for (int index = 0; index != 15; ++index)
        state = update_accumulator(state, observation(index), config, index).state;
    const auto fifteen = assess_accumulator(*state, config);
    CHECK(fifteen->status == "abstain");
    CHECK(fifteen->reason == "minimum_effective_samples");

    state = update_accumulator(state, observation(15), config, 15).state;
    const auto sixteen = assess_accumulator(*state, config);
    CHECK(sixteen->status == "accept");
    CHECK(sixteen->reason == "causal_lower_bound");
    CHECK(sixteen->causal_lower_bound > config.chance_rate + config.accept_margin);
    CHECK(sixteen->effective_sample_size == 16.0);
    CHECK(sixteen->revision == 16);
    CHECK(sixteen->evidence_addresses.front() == "evidence:0");
    CHECK(sixteen->evidence_addresses[2] == "evidence:10");
}

void check_random_reject() {
    const EvidenceAccumulatorConfig config;
    auto state = EvidenceAccumulatorState::empty("rule-a", config);
    for (int index = 0; index != 32; ++index) {
        state = update_accumulator(
            state, observation(index, index % 5 == 0 ? "support" : "refute"),
            config, index).state;
    }
    const auto decision = assess_accumulator(*state, config);
    CHECK(decision->status == "reject");
    CHECK(decision->reason == "upper_bound_below_threshold");
}

void check_duplicates_and_correlated_groups() {
    const EvidenceAccumulatorConfig config;
    auto state = EvidenceAccumulatorState::empty("rule-a", config);
    const auto first = observation(0, "support", "rule-a", {}, "one", "same", "same");
    state = update_accumulator(state, first, config, 0).state;
    const auto baseline = assess_accumulator(*state, config);
    for (int index = 0; index != 100; ++index) {
        const auto update = update_accumulator(
            state, observation(index + 1, "support", "rule-a", {}, "one", "same", "same"),
            config, index + 1);
        CHECK(update.state == state);
        CHECK(update.previous_decision == update.decision);
        CHECK(!update.applied);
        CHECK(update.reason == "duplicate");
    }
    CHECK(assess_accumulator(*state, config)->effective_sample_size ==
          baseline->effective_sample_size);

    auto correlated = EvidenceAccumulatorState::empty("rule-a", config);
    for (int index = 0; index != 100; ++index) {
        correlated = update_accumulator(
            correlated, observation(index, "support", "rule-a", {}, "one", "same"),
            config, index).state;
    }
    const auto decision = assess_accumulator(*correlated, config);
    CHECK(decision->effective_sample_size == 4.0);
    CHECK(decision->status == "abstain");
}

void check_revocation_and_reacceptance() {
    const EvidenceAccumulatorConfig config;
    auto state = accepted_state();
    int revoked_after = 0;
    for (int offset = 0; offset != 8; ++offset) {
        state = update_accumulator(
            state, observation(16 + offset, "refute"), config, 16 + offset).state;
        if (assess_accumulator(*state, config)->status != "accept") {
            revoked_after = offset + 1;
            break;
        }
    }
    CHECK(revoked_after >= 1);
    CHECK(revoked_after <= 4);
    const int start = 16 + revoked_after;
    for (int offset = 0; offset != 8; ++offset)
        state = update_accumulator(
            state, observation(start + offset), config, start + offset).state;
    CHECK(assess_accumulator(*state, config)->status == "accept");
}

void check_rejected_updates_retain_identity() {
    const EvidenceAccumulatorConfig config;
    const auto state = EvidenceAccumulatorState::empty("rule-a", config);
    const auto expired = update_accumulator(
        state, observation(0, "support", "rule-a", {}, {}, {}, {}, {}, 0.5, 1),
        config, 2);
    CHECK(expired.state == state);
    CHECK(expired.previous_decision == expired.decision);
    CHECK(expired.reason == "expired");

    const auto insufficient = update_accumulator(
        state, observation(1, "insufficient"), config, 1);
    CHECK(insufficient.state == state);
    CHECK(insufficient.previous_decision == insufficient.decision);
    CHECK(insufficient.reason == "insufficient");

    // Equality with expires_at is deliberately current, matching Python's >.
    const auto boundary = update_accumulator(
        state, observation(1, "support", "rule-a", {}, {}, {}, {}, {}, 0.5, 1),
        config, 1);
    CHECK(boundary.applied);

    // The Python source does not reject a negative current_step or require it
    // to follow observed_at. Keep that absence of a stronger condition.
    const auto negative_step = update_accumulator(
        state, observation(2), config, -1);
    CHECK(negative_step.applied);
}

void check_authority_and_value_copy() {
    const EvidenceAccumulatorConfig config;
    const auto state = accepted_state();
    const auto copied = std::make_shared<const EvidenceAccumulatorState>(*state);
    CHECK(assess_accumulator(*copied, config)->status == "accept");

    const EvidenceAccumulatorState foreign("rule-a", config);
    bool rejected = false;
    try {
        static_cast<void>(assess_accumulator(foreign, config));
    } catch (const AuthorityError&) {
        rejected = true;
    }
    CHECK(rejected);

    const auto authoritative_decision = assess_accumulator(*state, config);
    CHECK(is_authoritative_accumulator_decision(*authoritative_decision));
    const AccumulatorDecision copied_decision(*authoritative_decision);
    CHECK(is_authoritative_accumulator_decision(copied_decision));
    const AccumulatorDecision forged(
        "accept", "causal_lower_bound", 1.0, 1.0, 1.0, 100.0,
        100, 100, 0.0, 16, "rule-a", {"evidence:0"});
    CHECK(!is_authoritative_accumulator_decision(forged));
}

void check_producer_limited_diversity() {
    const EvidenceAccumulatorConfig config;
    auto state = EvidenceAccumulatorState::empty("rule-a", config);
    for (int index = 0; index != 16; ++index) {
        state = update_accumulator(
            state,
            observation(index, "support", "rule-a", {},
                        "forged-source:" + std::to_string(index),
                        "forged-context:" + std::to_string(index), {}, "one-producer"),
            config, index).state;
    }
    const auto decision = assess_accumulator(*state, config);
    CHECK(decision->source_diversity == 1);
    CHECK(decision->context_diversity == 1);
    CHECK(decision->status == "abstain");
    CHECK(decision->reason == "source_diversity");
}

void check_provenance_hash_and_numeric_values() {
    const auto item = observation(
        0, "support", "rule-a", "observational", "source:0", "context:0",
        "evidence:0", "producer:0", 0.5,
        std::nullopt, "sha256:natural-source", "revision:1");
    item.validate();
    CHECK(item.proposal_hash() ==
          "84f8490790f79e044298cf7ef20f995829cefd232bd55ed3ba96baee36a2470b");

    const auto [lower, upper] = wilson_interval(4.0, 0.0, 0.9);
    CHECK(std::abs(lower - 0.5965213747972955) < 1e-14);
    CHECK(upper == 1.0);
}

}  // namespace

int main() {
    check_fifteen_abstain_sixteen_accept();
    check_random_reject();
    check_duplicates_and_correlated_groups();
    check_revocation_and_reacceptance();
    check_rejected_updates_retain_identity();
    check_authority_and_value_copy();
    check_producer_limited_diversity();
    check_provenance_hash_and_numeric_values();
    std::cout << "PASS evidence accumulator parity, authority, and immutable updates\n";
}
