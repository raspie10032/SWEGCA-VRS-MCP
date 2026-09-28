#include "world/re_evidence_transaction.hpp"
#include "world/bounded_world_write.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace swegca::world;

void check(const bool condition, const char* expression, const int line) {
    if (!condition) {
        std::cerr << "CHECK failed at line " << line << ": " << expression
                  << '\n';
        std::exit(1);
    }
}

#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

template <typename Error, typename Operation>
void rejects(Operation&& operation, const std::string_view expected) {
    try {
        std::forward<Operation>(operation)();
    } catch (const Error& error) {
        CHECK(std::string_view(error.what()).find(expected) !=
              std::string_view::npos);
        return;
    } catch (...) {
        CHECK(false);
    }
    CHECK(false);
}

Tensor zeros(const TensorDType dtype,
             const std::vector<std::uint64_t>& shape) {
    std::size_t count = 1;
    for (const auto dimension : shape) {
        count *= static_cast<std::size_t>(dimension);
    }
    return Tensor(dtype, shape, std::vector<double>(count, 0.0));
}

std::shared_ptr<const WorldState> world(
    const TensorDType dtype = TensorDType::float32,
    const std::uint64_t batch = 1) {
    return std::make_shared<const WorldState>(
        zeros(dtype, {batch, 32, 4}),
        BooleanMask({batch, 32},
                    std::vector<std::uint8_t>(
                        static_cast<std::size_t>(batch * 32), 0)),
        BooleanMask({batch, 32},
                    std::vector<std::uint8_t>(
                        static_cast<std::size_t>(batch * 32), 0)),
        "test");
}

std::shared_ptr<const CognitiveState> cognitive(
    const TensorDType dtype = TensorDType::float32,
    const std::uint64_t batch = 1) {
    return std::make_shared<const CognitiveState>(
        zeros(dtype, {batch, 20, 4}), zeros(dtype, {batch, 6, 4}),
        zeros(dtype, {batch, 6, 4}));
}

EvidenceAccumulatorConfig accumulator_config() {
    return EvidenceAccumulatorConfig(
        0.2, 0.25, 0.9, 1.0, 1.0, 4, 2, 1, 4, 6, 4, 0.3,
        {"observational"});
}

struct AcceptedFixture final {
    EvidenceAccumulatorConfig config;
    std::shared_ptr<const EvidenceAccumulatorState> state;
    std::shared_ptr<const AccumulatorDecision> decision;
    std::vector<EvidenceObservation> observations;
    std::vector<ExperienceVectorBinding> bindings;
};

AcceptedFixture accepted_fixture() {
    auto config = accumulator_config();
    auto state = EvidenceAccumulatorState::empty("alpha-supports-beta", config);
    std::vector<EvidenceObservation> observations;
    std::vector<ExperienceVectorBinding> bindings;
    for (int index = 0; index < 4; ++index) {
        observations.emplace_back(
            "alpha-supports-beta", "evidence:" + std::to_string(index),
            "source-family:" + std::to_string(index),
            "context:" + std::to_string(index), "observational", "support",
            index, std::nullopt, "producer:" + std::to_string(index), 1.0,
            "source:" + std::to_string(index),
            "revision:" + std::to_string(index));
        const auto update = update_accumulator(
            state, observations.back(), config, index);
        CHECK(update.applied);
        state = update.state;
        std::vector<double> values(4, 0.0);
        values[static_cast<std::size_t>(index)] = 1.0;
        bindings.emplace_back(
            observations.back().source_address,
            observations.back().source_revision,
            std::vector<std::string>{observations.back().evidence_address},
            std::move(values));
    }
    auto decision = assess_accumulator(*state, config);
    CHECK(decision->status == "accept");
    return {std::move(config), std::move(state), std::move(decision),
            std::move(observations), std::move(bindings)};
}

void check_proposal(const SynapseProposal& proposal,
                    const AcceptedFixture& fixture) {
    CHECK(proposal.source == "accepted-experience");
    CHECK(proposal.hypothesis_id == "alpha-supports-beta");
    CHECK(proposal.evidence_addresses.size() == 1);
    CHECK(proposal.evidence_addresses[0] ==
          fixture.decision->evidence_addresses);
    const auto shape = proposal.delta_candidate.shape();
    CHECK(shape.size() == 3);
    CHECK(shape[0] == 1);
    CHECK(shape[1] == 32);
    CHECK(shape[2] == 4);
    for (std::uint64_t slot = 0; slot < 32; ++slot) {
        CHECK(proposal.target_slot_mask.at(0, slot) == (slot == 30));
        for (std::uint64_t dimension = 0; dimension < 4; ++dimension) {
            const auto index = static_cast<std::size_t>(slot * 4 + dimension);
            const double expected = slot == 30 ? 0.25 : 0.0;
            CHECK(proposal.delta_candidate.values()[index] == expected);
        }
    }
    CHECK(proposal.confidence.size() == 1);
    CHECK(proposal.contradiction.size() == 1);
    CHECK(proposal.uncertainty.size() == 1);
    CHECK(proposal.confidence[0] > proposal.contradiction[0]);
}

void test_world_and_cognitive_overloads() {
    const auto fixture = accepted_fixture();
    const auto world_proposal =
        experience_delta_proposal_from_accepted_observations(
            world(), *fixture.decision, fixture.observations, fixture.bindings,
            "accepted-experience");
    check_proposal(world_proposal, fixture);
    const auto cognitive_proposal =
        experience_delta_proposal_from_accepted_observations(
            cognitive(), *fixture.decision, fixture.observations,
            fixture.bindings, "accepted-experience");
    check_proposal(cognitive_proposal, fixture);

    std::vector<ExperienceVectorBinding> changed_bindings;
    changed_bindings.emplace_back("source:0", "revision:0",
                                  std::vector<std::string>{"evidence:0"},
                                  std::vector<double>{-1.0, 0.0, 0.0, 0.0});
    for (std::size_t index = 1; index < fixture.bindings.size(); ++index) {
        changed_bindings.push_back(fixture.bindings[index]);
    }
    const auto changed = experience_delta_proposal_from_accepted_observations(
        cognitive(), *fixture.decision, fixture.observations, changed_bindings,
        "accepted-experience");
    CHECK(!changed.delta_candidate.exact_equal(
        cognitive_proposal.delta_candidate));
}

void test_binding_validation() {
    rejects<std::invalid_argument>(
        [] { ExperienceVectorBinding("", "r", {"e"}, {1.0}); },
        "source provenance");
    rejects<std::invalid_argument>(
        [] { ExperienceVectorBinding("s", "", {"e"}, {1.0}); },
        "source provenance");
    rejects<std::invalid_argument>(
        [] { ExperienceVectorBinding("s", "r", {}, {1.0}); },
        "evidence addresses");
    rejects<std::invalid_argument>(
        [] { ExperienceVectorBinding("s", "r", {"e", "e"}, {1.0}); },
        "evidence addresses");
    rejects<std::invalid_argument>(
        [] { ExperienceVectorBinding("s", "r", {"e"}, {}); },
        "finite and nonempty");
    rejects<std::invalid_argument>(
        [] {
            ExperienceVectorBinding("s", "r", {"e"},
                                    {std::numeric_limits<double>::infinity()});
        },
        "finite and nonempty");
    rejects<std::invalid_argument>(
        [] { ExperienceVectorBinding("s", "r", {"e"}, {0.0, -0.0}); },
        "must be nonzero");
    const ExperienceVectorBinding whitespace(" ", "\t", {""}, {1.0});
    CHECK(whitespace.source_address == " ");
}

void test_authority_status_and_accepted_set_gates() {
    const auto fixture = accepted_fixture();
    const AccumulatorDecision forged(
        "accept", "forged", 1.0, 1.0, 1.0, 4.0, 4, 4, 0.0, 4,
        "alpha-supports-beta",
        {"evidence:0", "evidence:1", "evidence:2", "evidence:3"});
    rejects<AuthorityError>(
        [&] {
            static_cast<void>(
                experience_delta_proposal_from_accepted_observations(
                    world(), forged, fixture.observations, fixture.bindings,
                    "accepted-experience"));
        },
        "authority capability");

    const auto empty = EvidenceAccumulatorState::empty(
        "alpha-supports-beta", fixture.config);
    const auto abstain = assess_accumulator(*empty, fixture.config);
    rejects<AuthorityError>(
        [&] {
            static_cast<void>(
                experience_delta_proposal_from_accepted_observations(
                    world(), *abstain, {}, {}, "accepted-experience"));
        },
        "requires accumulator acceptance");

    auto duplicate = fixture.observations;
    duplicate.push_back(duplicate.front());
    rejects<std::invalid_argument>(
        [&] {
            static_cast<void>(
                experience_delta_proposal_from_accepted_observations(
                    world(), *fixture.decision, duplicate, fixture.bindings,
                    "accepted-experience"));
        },
        "accepted and admitted evidence sets differ");

    auto missing = fixture.observations;
    missing.pop_back();
    rejects<std::invalid_argument>(
        [&] {
            static_cast<void>(
                experience_delta_proposal_from_accepted_observations(
                    world(), *fixture.decision, missing, fixture.bindings,
                    "accepted-experience"));
        },
        "accepted and admitted evidence sets differ");
}

void test_provenance_binding_outcome_and_dimension_gates() {
    const auto fixture = accepted_fixture();
    std::vector<EvidenceObservation> incomplete;
    incomplete.emplace_back(
        fixture.observations[0].hypothesis_id,
        fixture.observations[0].evidence_address,
        fixture.observations[0].source_family,
        fixture.observations[0].context_hash, fixture.observations[0].axis,
        fixture.observations[0].outcome, fixture.observations[0].observed_at,
        fixture.observations[0].expires_at,
        fixture.observations[0].producer_id,
        fixture.observations[0].producer_confidence, "", "");
    for (std::size_t index = 1; index < fixture.observations.size(); ++index) {
        incomplete.push_back(fixture.observations[index]);
    }
    rejects<std::invalid_argument>(
        [&] {
            static_cast<void>(
                experience_delta_proposal_from_accepted_observations(
                    world(), *fixture.decision, incomplete, fixture.bindings,
                    "accepted-experience"));
        },
        "lack verified provenance");

    auto missing_binding = fixture.bindings;
    missing_binding.pop_back();
    rejects<std::invalid_argument>(
        [&] {
            static_cast<void>(
                experience_delta_proposal_from_accepted_observations(
                    world(), *fixture.decision, fixture.observations,
                    missing_binding, "accepted-experience"));
        },
        "source/revision bindings differ");

    auto duplicate_binding = fixture.bindings;
    duplicate_binding.push_back(duplicate_binding.front());
    rejects<std::invalid_argument>(
        [&] {
            static_cast<void>(
                experience_delta_proposal_from_accepted_observations(
                    world(), *fixture.decision, fixture.observations,
                    duplicate_binding, "accepted-experience"));
        },
        "source/revision bindings differ");

    std::vector<ExperienceVectorBinding> evidence_mismatch;
    evidence_mismatch.emplace_back(
        "source:0", "revision:0", std::vector<std::string>{"foreign"},
        std::vector<double>{1.0, 0.0, 0.0, 0.0});
    for (std::size_t index = 1; index < fixture.bindings.size(); ++index) {
        evidence_mismatch.push_back(fixture.bindings[index]);
    }
    rejects<std::invalid_argument>(
        [&] {
            static_cast<void>(
                experience_delta_proposal_from_accepted_observations(
                    world(), *fixture.decision, fixture.observations,
                    evidence_mismatch, "accepted-experience"));
        },
        "evidence binding differs");

    std::vector<EvidenceObservation> conflicting;
    conflicting.push_back(fixture.observations[0]);
    conflicting.emplace_back(
        fixture.observations[1].hypothesis_id,
        fixture.observations[1].evidence_address,
        fixture.observations[1].source_family,
        fixture.observations[1].context_hash, fixture.observations[1].axis,
        "refute", fixture.observations[1].observed_at,
        fixture.observations[1].expires_at,
        fixture.observations[1].producer_id,
        fixture.observations[1].producer_confidence,
        fixture.observations[0].source_address,
        fixture.observations[0].source_revision);
    conflicting.push_back(fixture.observations[2]);
    conflicting.push_back(fixture.observations[3]);
    std::vector<ExperienceVectorBinding> grouped;
    grouped.emplace_back("source:0", "revision:0",
                         std::vector<std::string>{"evidence:0", "evidence:1"},
                         std::vector<double>{1.0, 0.0, 0.0, 0.0});
    grouped.push_back(fixture.bindings[2]);
    grouped.push_back(fixture.bindings[3]);
    rejects<std::invalid_argument>(
        [&] {
            static_cast<void>(
                experience_delta_proposal_from_accepted_observations(
                    world(), *fixture.decision, conflicting, grouped,
                    "accepted-experience"));
        },
        "conflicting current outcomes");

    std::vector<ExperienceVectorBinding> wrong_dimension;
    wrong_dimension.emplace_back(
        "source:0", "revision:0", std::vector<std::string>{"evidence:0"},
        std::vector<double>{1.0, 0.0, 0.0});
    for (std::size_t index = 1; index < fixture.bindings.size(); ++index) {
        wrong_dimension.push_back(fixture.bindings[index]);
    }
    rejects<std::invalid_argument>(
        [&] {
            static_cast<void>(
                experience_delta_proposal_from_accepted_observations(
                    world(), *fixture.decision, fixture.observations,
                    wrong_dimension, "accepted-experience"));
        },
        "dimension differs");
}

void test_refute_sign_zero_cancellation_dtype_and_batch_boundary() {
    const auto fixture = accepted_fixture();
    std::vector<EvidenceObservation> one_refute;
    for (std::size_t index = 0; index < fixture.observations.size(); ++index) {
        const auto& original = fixture.observations[index];
        one_refute.emplace_back(
            original.hypothesis_id, original.evidence_address,
            original.source_family, original.context_hash, original.axis,
            index == 0 ? "refute" : "support", original.observed_at,
            original.expires_at, original.producer_id,
            original.producer_confidence, original.source_address,
            original.source_revision);
    }
    const auto signed_proposal =
        experience_delta_proposal_from_accepted_observations(
            world(), *fixture.decision, one_refute, fixture.bindings,
            "accepted-experience");
    const auto verification = static_cast<std::size_t>(30 * 4);
    CHECK(signed_proposal.delta_candidate.values()[verification] == -0.25);
    CHECK(signed_proposal.delta_candidate.values()[verification + 1] == 0.25);

    std::vector<EvidenceObservation> observations;
    std::vector<ExperienceVectorBinding> same_vectors;
    for (std::size_t index = 0; index < fixture.observations.size(); ++index) {
        const auto& original = fixture.observations[index];
        observations.emplace_back(
            original.hypothesis_id, original.evidence_address,
            original.source_family, original.context_hash, original.axis,
            index >= 2 ? "refute" : "support", original.observed_at,
            original.expires_at, original.producer_id,
            original.producer_confidence, original.source_address,
            original.source_revision);
        same_vectors.emplace_back(
            original.source_address, original.source_revision,
            std::vector<std::string>{original.evidence_address},
            std::vector<double>{1.0, 0.0, 0.0, 0.0});
    }
    rejects<std::invalid_argument>(
        [&] {
            static_cast<void>(
                experience_delta_proposal_from_accepted_observations(
                    world(), *fixture.decision, observations, same_vectors,
                    "accepted-experience"));
        },
        "no finite directional delta");

    const auto half = experience_delta_proposal_from_accepted_observations(
        world(TensorDType::float16), *fixture.decision, fixture.observations,
        fixture.bindings, "accepted-experience");
    CHECK(half.delta_candidate.dtype() == TensorDType::float16);
    CHECK(half.delta_candidate.values()[static_cast<std::size_t>(30 * 4)] ==
          0.25);

    rejects<std::invalid_argument>(
        [&] {
            static_cast<void>(
                experience_delta_proposal_from_accepted_observations(
                    world(TensorDType::float32, 2), *fixture.decision,
                    fixture.observations, fixture.bindings,
                    "accepted-experience"));
        },
        "evidence delta must have shape");
}

MainReEvidenceReceipt transaction_receipt(
    const int index, std::string verdict = "support",
    const bool complete = true, std::string candidate_source = {}) {
    const std::string evidence_ref = "natural:" + std::to_string(index);
    const std::string source_address = "source:" + std::to_string(index);
    const std::string revision = "revision:" + std::to_string(index);
    if (candidate_source.empty()) candidate_source = source_address;
    CurrentEvidenceDisposition current{
        evidence_ref, "current observation", "natural_observation",
        complete ? std::optional<std::string>(source_address) : std::nullopt,
        complete ? std::optional<std::string>(revision) : std::nullopt,
        complete ? std::optional<std::string>("family:" + std::to_string(index))
                 : std::nullopt,
        complete ? std::optional<std::string>("context:" + std::to_string(index))
                 : std::nullopt,
        complete ? std::optional<std::string>(
                       std::vector<std::string>{"observational", "counterfactual",
                                                "intervention", "cross_context"}
                           [static_cast<std::size_t>(index) % 4])
                 : std::nullopt,
        complete ? std::optional<std::string>(verdict) : std::nullopt,
        complete ? std::optional<std::int64_t>(index) : std::nullopt,
        std::nullopt,
        complete ? std::optional<std::string>("producer:" + std::to_string(index))
                 : std::nullopt,
        complete ? std::optional<double>(1.0) : std::nullopt,
        complete, complete ? "present" : "unavailable_in_frozen_input"};
    const bool conflict = verdict == "conflict";
    const bool insufficient = verdict == "insufficient";
    return MainReEvidenceReceipt(
        "rozephine-main-re-evidence-receipt-v2", "sole-main",
        "request:" + std::to_string(index), "alpha beta", "episode:direct",
        "alpha supports beta", verdict, "current evidence was re-evaluated",
        {evidence_ref}, {std::move(current)}, {"memory:evidence"},
        {CandidateDisposition{
            "episode:direct", true, {"memory:evidence"},
            {std::move(candidate_source)}, "present", "historical_only",
            revision, "present", std::nullopt}},
        {ProposalDisposition{"worker", "model-output:" + std::to_string(index),
                             true, std::nullopt}},
        {"worker"}, false, 10, conflict, insufficient,
        conflict || insufficient, !conflict && !insufficient);
}

void test_re_evidence_admission_and_event_provenance() {
    const EvidenceAccumulatorConfig config;
    auto state = EvidenceAccumulatorState::empty("alpha-supports-beta", config);
    std::vector<EvidenceObservation> accepted_observations;
    std::vector<ExperienceVectorBinding> accepted_vectors;
    const auto first = admit_re_evidence_to_accumulator(
        transaction_receipt(0), state, config, "alpha-supports-beta",
        "event:0", 0);
    CHECK(first.admitted);
    CHECK(first.event.has_value());
    CHECK(first.event->evidence_kind() == EvidenceKind::observed_evidence);
    CHECK(first.event->source().source_ref == "source:0");
    CHECK(first.event->evidence_refs().size() == 2);
    CHECK(std::find(first.event->evidence_refs().begin(),
                    first.event->evidence_refs().end(), "memory:evidence") ==
          first.event->evidence_refs().end());
    CHECK(std::find(first.event->evidence_refs().begin(),
                    first.event->evidence_refs().end(), "model-output:0") ==
          first.event->evidence_refs().end());
    const auto& metadata = first.event->metadata();
    CHECK(metadata.at("replay_evidence_refs_excluded").as_array().size() == 1);
    CHECK(metadata.at("proposal_addresses_excluded").as_array().size() == 1);
    CHECK(std::get<bool>(metadata.at("world_write_authority").storage()) == false);
    CHECK(first.observations[0].source_revision == "revision:0");
    CHECK(!first.world_write_eligible);
    state = first.state;
    accepted_observations.push_back(first.observations.front());
    accepted_vectors.emplace_back(
        first.observations.front().source_address,
        first.observations.front().source_revision,
        std::vector<std::string>{first.observations.front().evidence_address},
        std::vector<double>{1.0, 0.0, 0.0, 0.0});

    std::shared_ptr<const AccumulatorDecision> final_decision;
    bool final_eligible = false;
    for (int index = 1; index < 16; ++index) {
        const auto next = admit_re_evidence_to_accumulator(
            transaction_receipt(index), state, config, "alpha-supports-beta",
            "event:" + std::to_string(index), index);
        state = next.state;
        final_decision = next.decision;
        final_eligible = next.world_write_eligible;
        accepted_observations.push_back(next.observations.front());
        std::vector<double> values(4, 0.0);
        values[static_cast<std::size_t>(index) % 4] = 1.0;
        accepted_vectors.emplace_back(
            next.observations.front().source_address,
            next.observations.front().source_revision,
            std::vector<std::string>{next.observations.front().evidence_address},
            std::move(values));
    }
    CHECK(final_decision != nullptr);
    CHECK(final_decision->status == "accept");
    CHECK(final_eligible);
    CHECK(is_authoritative_accumulator_decision(*final_decision));

    const auto single_world = cognitive();
    const auto before = cognitive_state_hash(*single_world);
    const auto proposal = experience_delta_proposal_from_accepted_observations(
        single_world, *final_decision, accepted_observations, accepted_vectors,
        "accepted-re-evidence-experience");
    const auto gates = world_write_gates_from_decision(
        *final_decision, proposal, true, true, true, false, true, true, true,
        true, true, true);
    const auto committed = bounded_verification_write(
        single_world, proposal, gates, BoundedWorldWriteConfig(), true);
    CHECK(committed.committed);
    CHECK(committed.receipt != nullptr);
    const auto rolled_back = rollback_bounded_verification_write(
        committed.state, *committed.receipt);
    CHECK(cognitive_state_hash(*rolled_back) == before);
    CHECK(rolled_back->exact_equal(*single_world));
}

void test_admission_abstention_and_preflight_gates() {
    const auto config = accumulator_config();
    const auto state = EvidenceAccumulatorState::empty("alpha-supports-beta", config);
    for (const auto& verdict : {std::string("conflict"), std::string("insufficient")}) {
        const auto admission = admit_re_evidence_to_accumulator(
            transaction_receipt(0, verdict, false), state, config,
            "alpha-supports-beta", "", 0);
        CHECK(!admission.admitted);
        CHECK(admission.state == state);
        CHECK(!admission.event.has_value());
        CHECK(admission.decision == nullptr);
        CHECK(!admission.world_write_eligible);
    }
    rejects<std::invalid_argument>(
        [&] {
            static_cast<void>(admit_re_evidence_to_accumulator(
                transaction_receipt(0), state, config, "foreign", "event", 0));
        },
        "hypothesis differ");
    rejects<std::invalid_argument>(
        [&] {
            static_cast<void>(admit_re_evidence_to_accumulator(
                transaction_receipt(0), state, config, "alpha-supports-beta",
                "event", -1));
        },
        "nonnegative");
    rejects<std::invalid_argument>(
        [&] {
            static_cast<void>(admit_re_evidence_to_accumulator(
                transaction_receipt(0, "support", false), state, config,
                "alpha-supports-beta", "event", 0));
        },
        "provenance is incomplete");
    rejects<std::invalid_argument>(
        [&] {
            static_cast<void>(admit_re_evidence_to_accumulator(
                transaction_receipt(0, "support", true, "foreign-source"),
                state, config, "alpha-supports-beta", "event", 0));
        },
        "not exactly bound");

    const auto once = admit_re_evidence_to_accumulator(
        transaction_receipt(0), state, config, "alpha-supports-beta", "event", 0);
    rejects<std::invalid_argument>(
        [&] {
            static_cast<void>(admit_re_evidence_to_accumulator(
                transaction_receipt(0), once.state, config,
                "alpha-supports-beta", "event:duplicate", 0));
        },
        "already accumulated");
}

}  // namespace

int main() {
    test_world_and_cognitive_overloads();
    test_binding_validation();
    test_authority_status_and_accepted_set_gates();
    test_provenance_binding_outcome_and_dimension_gates();
    test_refute_sign_zero_cancellation_dtype_and_batch_boundary();
    test_re_evidence_admission_and_event_provenance();
    test_admission_abstention_and_preflight_gates();
    std::cout << "re-evidence transaction tests passed\n";
}
