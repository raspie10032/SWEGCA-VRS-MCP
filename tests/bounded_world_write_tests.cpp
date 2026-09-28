#include "world/bounded_world_write.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace swegca::world {
bool bounded_world_write_payload_matches_for_test(
    const EvidenceRevisionVerification&, const AccumulatorDecision&);
}

namespace {

using namespace swegca::world;

void check(const bool condition, const char* expression, const int line) {
    if (!condition) {
        std::cerr << "CHECK failed at line " << line << ": " << expression << '\n';
        std::exit(1);
    }
}

#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

template <class Exception, class Function>
void check_throws(Function&& function) {
    bool rejected = false;
    try {
        function();
    } catch (const Exception&) {
        rejected = true;
    }
    CHECK(rejected);
}

std::shared_ptr<const CognitiveState> empty_state() {
    return std::make_shared<const CognitiveState>(
        Tensor(TensorDType::float32, {1, 20, 32}, std::vector<double>(640, 0.0)),
        Tensor(TensorDType::float32, {1, 6, 32}, std::vector<double>(192, 0.0)),
        Tensor(TensorDType::float32, {1, 6, 32}, std::vector<double>(192, 0.0)));
}

std::shared_ptr<const CognitiveState> malformed_state(
    const std::vector<std::uint64_t>& semantic_shape,
    const std::vector<std::uint64_t>& executive_shape,
    const std::vector<std::uint64_t>& scratch_shape) {
    const auto count = [](const std::vector<std::uint64_t>& shape) {
        std::size_t result = 1;
        for (const auto dimension : shape) result *= static_cast<std::size_t>(dimension);
        return result;
    };
    return std::make_shared<const CognitiveState>(
        Tensor(TensorDType::float32, semantic_shape,
               std::vector<double>(count(semantic_shape), 0.0)),
        Tensor(TensorDType::float32, executive_shape,
               std::vector<double>(count(executive_shape), 0.0)),
        Tensor(TensorDType::float32, scratch_shape,
               std::vector<double>(count(scratch_shape), 0.0)));
}

SynapseProposal proposal(const std::shared_ptr<const CognitiveState>& state,
                         ProposalTargetSlot target = std::string("verification"),
                         Tensor logits = Tensor(TensorDType::float32, {1, 2}, {8.0, -8.0})) {
    return evidence_delta_proposal(
        state, Tensor(TensorDType::float32, {1, 32}, std::vector<double>(32, 1.0)),
        logits, "controlled-evidence",
        {{"world-write-evidence:0", "world-write-evidence:1"}},
        "world-write-test", std::move(target));
}

std::shared_ptr<const AccumulatorDecision> decision(const std::string& status = "accept") {
    const EvidenceAccumulatorConfig config;
    auto state = EvidenceAccumulatorState::empty("world-write-test", config);
    const int count = status == "abstain" ? 2 : 16;
    for (int index = 0; index != count; ++index) {
        const EvidenceObservation item(
            "world-write-test", "world-write-evidence:" + std::to_string(index),
            "world-write-source:" + std::to_string(index % 2),
            "world-write-context:" + std::to_string(index),
            config.required_axes[static_cast<std::size_t>(index) % config.required_axes.size()],
            "support", index, std::nullopt,
            "world-write-producer:" + std::to_string(index));
        state = update_accumulator(state, item, config, index).state;
    }
    return assess_accumulator(*state, config);
}

struct GateFlags final {
    bool definitions_complete{true};
    bool counterfactual_support{true};
    bool intervention_support{true};
    bool regime_change_suspected{false};
    bool slot_gate_passed{true};
    bool device_gate_passed{true};
    bool capacity_strategy_safe{true};
    bool evidence_current{true};
    bool accumulator_revision_current{true};
    bool runtime_context_safe{true};
};

WorldWriteGates gates(const SynapseProposal& bound_proposal,
                      const std::string& status = "accept",
                      const GateFlags& flags = {}) {
    const auto accepted = decision(status);
    return world_write_gates_from_decision(
        *accepted, bound_proposal, flags.definitions_complete,
        flags.counterfactual_support, flags.intervention_support,
        flags.regime_change_suspected, flags.slot_gate_passed,
        flags.device_gate_passed, flags.capacity_strategy_safe,
        flags.evidence_current, flags.accumulator_revision_current,
        flags.runtime_context_safe);
}

void check_authority_binds_hypothesis_evidence_and_proposal() {
    const auto state = empty_state();
    const auto valid = proposal(state);
    const auto authority = gates(valid);

    auto empty = valid;
    empty.evidence_addresses = {{}};
    auto foreign = valid;
    foreign.evidence_addresses = {{"foreign:evidence"}};
    auto wrong_hypothesis = valid;
    wrong_hypothesis.hypothesis_id = "other-hypothesis";
    for (const auto* candidate : {&empty, &foreign, &wrong_hypothesis})
        check_throws<AuthorityError>([&] { static_cast<void>(gates(*candidate)); });

    auto changed = valid;
    auto changed_values = std::vector<double>(changed.delta_candidate.values().begin(),
                                               changed.delta_candidate.values().end());
    changed_values[30U * 32U] = 0.5;
    changed.delta_candidate = Tensor(
        TensorDType::float32, {1, 32, 32}, std::move(changed_values));
    check_throws<AuthorityError>([&] {
        static_cast<void>(bounded_verification_write(
            state, changed, authority, BoundedWorldWriteConfig(), true));
    });
}

void check_bounded_commit_receipt_and_partition() {
    const auto state = empty_state();
    const auto candidate = proposal(state);
    const auto before = cognitive_state_hash(*state);
    const auto result = bounded_verification_write(
        state, candidate, gates(candidate), BoundedWorldWriteConfig(), true);
    CHECK(result.authorized);
    CHECK(result.committed);
    CHECK(result.reason == "committed");
    CHECK(result.receipt != nullptr);
    CHECK(result.state != state);
    CHECK(result.state->persistent_state_count() == 1);
    CHECK(cognitive_state_hash(*result.state) != before);
    CHECK(result.state->semantic_slots().exact_equal(state->semantic_slots()));
    CHECK(result.state->executive_slots().exact_equal(state->executive_slots()));

    const auto& delta = result.proposed_delta.values();
    double norm_squared = 0.0;
    for (std::size_t column = 0; column != 32; ++column) {
        const double value = delta[30U * 32U + column];
        norm_squared += value * value;
    }
    CHECK(std::sqrt(norm_squared) <= 0.02 + 1e-7);
    for (std::size_t index = 0; index != result.state->scratch_slots().values().size(); ++index) {
        const bool verification = index >= 4U * 32U && index < 5U * 32U;
        if (!verification)
            CHECK(result.state->scratch_slots().values()[index] ==
                  state->scratch_slots().values()[index]);
    }

    const auto& receipt = *result.receipt;
    CHECK(receipt.revision == 1);
    CHECK(receipt.target_role == "verification");
    CHECK(receipt.before_state_hash == before);
    CHECK(receipt.after_state_hash == cognitive_state_hash(*result.state));
    CHECK(receipt.before_slot.shape()[0] == 1);
    CHECK(receipt.before_slot.shape()[1] == 32);
    CHECK(receipt.evidence_refs == std::vector<std::string>({
        "world-write-evidence:0", "world-write-evidence:1"}));
    CHECK(receipt.hypothesis_id == "world-write-test");
    CHECK(!receipt.proposal_binding_digest.empty());
    CHECK(!receipt.receipt_id.empty());
    CHECK(!receipt.prior_write_metadata.has_value());
    const auto metadata = result.state->self_state().find("bounded_verification_write");
    CHECK(metadata != result.state->self_state().end());
    CHECK(metadata->second.at("receipt_id").as_string() == receipt.receipt_id);
    CHECK(metadata->second.at("revision").as_number() == 1.0);
}

std::shared_ptr<const CognitiveState> with_later_metadata(
    const CognitiveState& state, JsonValue::Object goal_state,
    JsonValue::Object value_state) {
    return std::make_shared<const CognitiveState>(
        state.semantic_slots().clone(), state.executive_slots().clone(),
        state.scratch_slots().clone(), state.structured_world_graph(),
        std::vector<std::string>(state.evidence_refs().begin(),
                                 state.evidence_refs().end()),
        std::move(goal_state), std::move(value_state), state.self_state(),
        std::string(state.owner_id()));
}

void check_receipt_roundtrip_and_exact_rollback() {
    const auto state = empty_state();
    const auto candidate = proposal(state);
    const auto result = bounded_verification_write(
        state, candidate, gates(candidate), BoundedWorldWriteConfig(), true);
    CHECK(result.receipt != nullptr);
    const auto payload = bounded_world_write_receipt_to_dict(*result.receipt);
    const auto restored_receipt = bounded_world_write_receipt_from_dict(payload);
    CHECK(restored_receipt.receipt_id == result.receipt->receipt_id);
    CHECK(restored_receipt.revision == result.receipt->revision);
    CHECK(restored_receipt.before_slot.exact_equal(result.receipt->before_slot));
    CHECK(restored_receipt.prior_write_metadata == result.receipt->prior_write_metadata);
    const auto restored = rollback_bounded_verification_write(
        result.state, restored_receipt);
    CHECK(cognitive_state_hash(*restored) == cognitive_state_hash(*state));
    CHECK(restored->scratch_slots().exact_equal(state->scratch_slots()));
    CHECK(restored->exact_equal(*state));

    const auto stale = with_later_metadata(
        *result.state, {{"changed", true}}, result.state->value_state());
    check_throws<std::invalid_argument>([&] {
        static_cast<void>(rollback_bounded_verification_write(
            stale, *result.receipt));
    });
}

void check_scoped_retraction_preserves_unrelated_cognition() {
    const auto state = empty_state();
    const auto candidate = proposal(state);
    const auto result = bounded_verification_write(
        state, candidate, gates(candidate), BoundedWorldWriteConfig(), true);
    CHECK(result.receipt != nullptr);
    const auto later_metadata = with_later_metadata(
        *result.state,
        {{"autonomy_phase", "observe"}, {"autonomy_step", 11}},
        {{"current_preference", "keep"}});
    auto later_self = later_metadata->self_state();
    later_self["unrelated_later_metadata"] = JsonValue::Object{{"step", 12}};
    const auto later = std::make_shared<const CognitiveState>(
        later_metadata->semantic_slots().clone(),
        later_metadata->executive_slots().clone(),
        later_metadata->scratch_slots().clone(),
        later_metadata->structured_world_graph(),
        std::vector<std::string>(later_metadata->evidence_refs().begin(),
                                 later_metadata->evidence_refs().end()),
        later_metadata->goal_state(), later_metadata->value_state(),
        std::move(later_self), std::string(later_metadata->owner_id()));
    const auto retracted = retract_bounded_verification_write(
        later, *result.receipt);
    CHECK(retracted->goal_state() == later->goal_state());
    CHECK(retracted->value_state() == later->value_state());
    CHECK(retracted->semantic_slots().exact_equal(later->semantic_slots()));
    CHECK(retracted->executive_slots().exact_equal(later->executive_slots()));
    for (std::size_t index = 0; index != retracted->scratch_slots().values().size(); ++index) {
        const bool verification = index >= 4U * 32U && index < 5U * 32U;
        if (verification)
            CHECK(retracted->scratch_slots().values()[index] ==
                  state->scratch_slots().values()[index]);
        else
            CHECK(retracted->scratch_slots().values()[index] ==
                  later->scratch_slots().values()[index]);
    }
    CHECK(!retracted->self_state().contains("bounded_verification_write"));
    CHECK(retracted->self_state().contains("unrelated_later_metadata"));
    CHECK(retracted->self_state().at("unrelated_later_metadata") ==
          later->self_state().at("unrelated_later_metadata"));
}

void check_scoped_retraction_requires_lifo_and_restores_prior_head() {
    const auto state = empty_state();
    const auto first_proposal = proposal(state);
    const auto first = bounded_verification_write(
        state, first_proposal, gates(first_proposal),
        BoundedWorldWriteConfig(), true);
    CHECK(first.receipt != nullptr);
    const auto second_proposal = proposal(first.state);
    const auto second = bounded_verification_write(
        first.state, second_proposal, gates(second_proposal),
        BoundedWorldWriteConfig(), true);
    CHECK(second.receipt != nullptr);
    CHECK(second.receipt->revision == 2);
    CHECK(second.receipt->prior_write_metadata.has_value());
    const auto second_roundtrip = bounded_world_write_receipt_from_dict(
        bounded_world_write_receipt_to_dict(*second.receipt));
    const auto strict_first = rollback_bounded_verification_write(
        second.state, second_roundtrip);
    CHECK(cognitive_state_hash(*strict_first) == cognitive_state_hash(*first.state));
    CHECK(strict_first->exact_equal(*first.state));
    check_throws<std::invalid_argument>([&] {
        static_cast<void>(retract_bounded_verification_write(
            second.state, *first.receipt));
    });
    const auto first_head = retract_bounded_verification_write(
        second.state, *second.receipt);
    const auto metadata = first_head->self_state().find("bounded_verification_write");
    CHECK(metadata != first_head->self_state().end());
    CHECK(metadata->second.at("receipt_id").as_string() == first.receipt->receipt_id);
    CHECK(metadata->second.at("revision").as_number() == 1.0);
    const auto baseline = retract_bounded_verification_write(
        first_head, *first.receipt);
    CHECK(baseline->scratch_slots().exact_equal(state->scratch_slots()));
    CHECK(!baseline->self_state().contains("bounded_verification_write"));
}

void check_retraction_rejects_unrelated_slot_and_tampered_receipt() {
    const auto state = empty_state();
    const auto candidate = proposal(state);
    const auto result = bounded_verification_write(
        state, candidate, gates(candidate), BoundedWorldWriteConfig(), true);
    CHECK(result.receipt != nullptr);
    auto scratch_values = std::vector<double>(result.state->scratch_slots().values().begin(),
                                              result.state->scratch_slots().values().end());
    scratch_values[4U * 32U] += 1.0;
    const auto changed_slot = std::make_shared<const CognitiveState>(
        result.state->semantic_slots().clone(), result.state->executive_slots().clone(),
        Tensor(TensorDType::float32, {1, 6, 32}, std::move(scratch_values)),
        result.state->structured_world_graph(),
        std::vector<std::string>(result.state->evidence_refs().begin(),
                                 result.state->evidence_refs().end()),
        result.state->goal_state(), result.state->value_state(), result.state->self_state(),
        std::string(result.state->owner_id()));
    check_throws<std::invalid_argument>([&] {
        validate_bounded_verification_retraction(*changed_slot, *result.receipt);
    });

    auto bad_values = std::vector<double>(result.receipt->before_slot.values().begin(),
                                          result.receipt->before_slot.values().end());
    bad_values[0] = 1.0;
    const BoundedWorldWriteReceipt tampered(
        result.receipt->receipt_id, result.receipt->revision,
        result.receipt->target_role, result.receipt->before_state_hash,
        result.receipt->after_state_hash,
        Tensor(TensorDType::float32, {1, 32}, std::move(bad_values)),
        result.receipt->before_slot_hash, result.receipt->after_slot_hash,
        result.receipt->applied_delta_hash, result.receipt->evidence_refs,
        result.receipt->prior_write_metadata, result.receipt->hypothesis_id,
        result.receipt->proposal_binding_digest);
    check_throws<std::invalid_argument>([&] {
        static_cast<void>(retract_bounded_verification_write(
            result.state, tampered));
    });
}

void check_malformed_topologies_fail_before_partition_indexing() {
    const auto valid = empty_state();
    const auto candidate = proposal(valid);
    const auto authority = gates(candidate);
    const auto committed = bounded_verification_write(
        valid, candidate, authority, BoundedWorldWriteConfig(), true);
    CHECK(committed.receipt != nullptr);

    const std::vector<std::shared_ptr<const CognitiveState>> malformed{
        // Total is 32, but global verification index 30 is outside scratch.
        malformed_state({1, 31, 32}, {1, 1, 32}, {1, 0, 32}),
        // The frozen compatibility profile has only 31 slots.
        malformed_state({1, 20, 32}, {1, 6, 32}, {1, 5, 32}),
    };
    for (const auto& state : malformed) {
        check_throws<std::invalid_argument>([&] {
            static_cast<void>(bounded_verification_write(
                state, candidate, authority, BoundedWorldWriteConfig(), true));
        });
        check_throws<std::invalid_argument>([&] {
            static_cast<void>(rollback_bounded_verification_write(
                state, *committed.receipt));
        });
        check_throws<std::invalid_argument>([&] {
            validate_bounded_verification_retraction(*state, *committed.receipt);
        });
        check_throws<std::invalid_argument>([&] {
            static_cast<void>(retract_bounded_verification_write(
                state, *committed.receipt));
        });
    }
    // CognitiveState itself now rejects malformed partition rank before this
    // API can receive it; keep that construction boundary under test too.
    check_throws<std::invalid_argument>([&] {
        static_cast<void>(malformed_state({20, 32}, {1, 6, 32}, {1, 6, 32}));
    });
}

void check_receipt_numeric_boundaries_and_python_scalar_coercion() {
    const auto state = empty_state();
    const auto candidate = proposal(state);
    const auto result = bounded_verification_write(
        state, candidate, gates(candidate), BoundedWorldWriteConfig(), true);
    CHECK(result.receipt != nullptr);
    auto payload = bounded_world_write_receipt_to_dict(*result.receipt);
    payload["revision"] = "  +1\t";
    payload["receipt_id"] = 17;
    payload["target_role"] = true;
    payload["before_state_hash"] = nullptr;
    payload["hypothesis_id"] = 3.0;
    const auto coerced = bounded_world_write_receipt_from_dict(payload);
    CHECK(coerced.revision == 1);
    CHECK(coerced.receipt_id == "17");
    CHECK(coerced.target_role == "True");
    CHECK(coerced.before_state_hash == "None");
    CHECK(coerced.hypothesis_id == "3.0");

    payload = bounded_world_write_receipt_to_dict(*result.receipt);
    payload["revision"] = 1.9;
    CHECK(bounded_world_write_receipt_from_dict(payload).revision == 1);
    payload["revision"] = std::numeric_limits<std::int64_t>::max();
    CHECK(bounded_world_write_receipt_from_dict(payload).revision ==
          std::numeric_limits<std::int64_t>::max());
    payload["revision"] = -9223372036854775808.0;
    CHECK(bounded_world_write_receipt_from_dict(payload).revision ==
          std::numeric_limits<std::int64_t>::min());

    for (const double invalid : {
             9223372036854775808.0,
             std::numeric_limits<double>::quiet_NaN(),
             std::numeric_limits<double>::infinity(),
             -std::numeric_limits<double>::infinity()}) {
        payload["revision"] = invalid;
        check_throws<std::invalid_argument>([&] {
            static_cast<void>(bounded_world_write_receipt_from_dict(payload));
        });
    }

    for (const double invalid : {
             9223372036854775808.0,
             std::numeric_limits<double>::quiet_NaN(),
             std::numeric_limits<double>::infinity()}) {
        auto self_state = state->self_state();
        self_state["bounded_verification_write"] =
            JsonValue::Object{{"revision", invalid}};
        const auto malformed_metadata = std::make_shared<const CognitiveState>(
            state->semantic_slots().clone(), state->executive_slots().clone(),
            state->scratch_slots().clone(), state->structured_world_graph(),
            std::vector<std::string>(state->evidence_refs().begin(),
                                     state->evidence_refs().end()),
            state->goal_state(), state->value_state(), std::move(self_state),
            std::string(state->owner_id()));
        const auto malformed_proposal = proposal(malformed_metadata);
        const auto malformed_gates = gates(malformed_proposal);
        check_throws<std::invalid_argument>([&] {
            static_cast<void>(bounded_verification_write(
                malformed_metadata, malformed_proposal, malformed_gates,
                BoundedWorldWriteConfig(), true));
        });
    }
}

void check_lifo_revision_distinguishes_values_above_double_integer_precision() {
    constexpr std::int64_t receipt_revision = 9'007'199'254'740'993LL;
    constexpr double metadata_revision = 9'007'199'254'740'992.0;
    const auto state = empty_state();
    const auto candidate = proposal(state);
    const auto result = bounded_verification_write(
        state, candidate, gates(candidate), BoundedWorldWriteConfig(), true);
    CHECK(result.receipt != nullptr);

    auto self_state = result.state->self_state();
    auto head = self_state.at("bounded_verification_write").as_object();
    head["revision"] = metadata_revision;
    self_state["bounded_verification_write"] = std::move(head);
    const auto imprecise_head = std::make_shared<const CognitiveState>(
        result.state->semantic_slots().clone(), result.state->executive_slots().clone(),
        result.state->scratch_slots().clone(), result.state->structured_world_graph(),
        std::vector<std::string>(result.state->evidence_refs().begin(),
                                 result.state->evidence_refs().end()),
        result.state->goal_state(), result.state->value_state(), std::move(self_state),
        std::string(result.state->owner_id()));
    const BoundedWorldWriteReceipt adjacent(
        result.receipt->receipt_id, receipt_revision, result.receipt->target_role,
        result.receipt->before_state_hash, result.receipt->after_state_hash,
        result.receipt->before_slot.clone(), result.receipt->before_slot_hash,
        result.receipt->after_slot_hash, result.receipt->applied_delta_hash,
        result.receipt->evidence_refs, result.receipt->prior_write_metadata,
        result.receipt->hypothesis_id, result.receipt->proposal_binding_digest);
    check_throws<std::invalid_argument>([&] {
        validate_bounded_verification_retraction(*imprecise_head, adjacent);
    });
}

EvidenceRevisionVerification payload_verification(
    const AccumulatorDecision& decision, RevisionPayloadValue revision,
    RevisionPayloadValue source_diversity,
    RevisionPayloadValue context_diversity) {
    return EvidenceRevisionVerification(
        true, true, {}, {}, {}, {}, std::nullopt, decision.hypothesis_id, {}, {}, {
            {"status", decision.status}, {"reason", decision.reason},
            {"posterior_mean", decision.posterior_mean},
            {"causal_lower_bound", decision.causal_lower_bound},
            {"overall_upper_bound", decision.overall_upper_bound},
            {"effective_sample_size", decision.effective_sample_size},
            {"source_diversity", std::move(source_diversity)},
            {"context_diversity", std::move(context_diversity)},
            {"regime_change_score", decision.regime_change_score},
            {"revision", std::move(revision)},
            {"hypothesis_id", decision.hypothesis_id},
            {"evidence_addresses", decision.evidence_addresses},
        });
}

void check_revision_verification_payload_large_integer_exactness() {
    constexpr std::uint64_t exact = 9'007'199'254'740'992ULL;
    constexpr std::uint64_t adjacent = exact + 1ULL;
    const AccumulatorDecision baseline(
        "accept", "accepted", 0.75, 0.6, 0.9, 16.0,
        static_cast<std::size_t>(exact), static_cast<std::size_t>(exact),
        0.0, exact, "world-write-test",
        {"world-write-evidence:0", "world-write-evidence:1"});
    const auto exact_integer = payload_verification(
        baseline, static_cast<std::int64_t>(exact),
        static_cast<std::int64_t>(exact), static_cast<std::int64_t>(exact));
    const auto exact_double = payload_verification(
        baseline, static_cast<double>(exact), static_cast<double>(exact),
        static_cast<double>(exact));
    CHECK(bounded_world_write_payload_matches_for_test(exact_integer, baseline));
    CHECK(bounded_world_write_payload_matches_for_test(exact_double, baseline));

    const AccumulatorDecision wrong_revision(
        baseline.status, baseline.reason, baseline.posterior_mean,
        baseline.causal_lower_bound, baseline.overall_upper_bound,
        baseline.effective_sample_size, baseline.source_diversity,
        baseline.context_diversity, baseline.regime_change_score, adjacent,
        baseline.hypothesis_id, baseline.evidence_addresses);
    CHECK(!bounded_world_write_payload_matches_for_test(exact_integer, wrong_revision));
    CHECK(!bounded_world_write_payload_matches_for_test(exact_double, wrong_revision));

    const AccumulatorDecision wrong_source(
        baseline.status, baseline.reason, baseline.posterior_mean,
        baseline.causal_lower_bound, baseline.overall_upper_bound,
        baseline.effective_sample_size, static_cast<std::size_t>(adjacent),
        baseline.context_diversity, baseline.regime_change_score, baseline.revision,
        baseline.hypothesis_id, baseline.evidence_addresses);
    const AccumulatorDecision wrong_context(
        baseline.status, baseline.reason, baseline.posterior_mean,
        baseline.causal_lower_bound, baseline.overall_upper_bound,
        baseline.effective_sample_size, baseline.source_diversity,
        static_cast<std::size_t>(adjacent), baseline.regime_change_score,
        baseline.revision, baseline.hypothesis_id, baseline.evidence_addresses);
    CHECK(!bounded_world_write_payload_matches_for_test(exact_integer, wrong_source));
    CHECK(!bounded_world_write_payload_matches_for_test(exact_integer, wrong_context));

    const AccumulatorDecision int64_edge(
        baseline.status, baseline.reason, baseline.posterior_mean,
        baseline.causal_lower_bound, baseline.overall_upper_bound,
        baseline.effective_sample_size,
        static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()),
        baseline.context_diversity, baseline.regime_change_score,
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()),
        baseline.hypothesis_id, baseline.evidence_addresses);
    const auto edge_payload = payload_verification(
        int64_edge, std::numeric_limits<std::int64_t>::max(),
        std::numeric_limits<std::int64_t>::max(), static_cast<std::int64_t>(exact));
    CHECK(bounded_world_write_payload_matches_for_test(edge_payload, int64_edge));
}

void check_cpp_proposal_vector_hash_profile() {
    const auto state = empty_state();
    const auto candidate = proposal(state);
    const auto result = bounded_verification_write(
        state, candidate, gates(candidate), BoundedWorldWriteConfig(), true);
    CHECK(result.receipt != nullptr);
    // confidence/contradiction/uncertainty are vector<double> in this API and
    // therefore hash as float64 independently of the delta tensor dtype.
    // Python proposals with scalar tensors of another dtype are outside this
    // C++ representation's exact digest parity profile.
    CHECK(result.receipt->proposal_binding_digest ==
          "2ca85a326ae81804ea671ba74b4f0e509e6ab964efd299b1130306378985625f");
}

void check_dry_run_preserves_shared_identity() {
    const auto state = empty_state();
    const auto candidate = proposal(state);
    const auto result = bounded_verification_write(
        state, candidate, gates(candidate), BoundedWorldWriteConfig(), false);
    CHECK(result.state == state);
    CHECK(result.authorized);
    CHECK(!result.committed);
    CHECK(result.reason == "authorized_dry_run");
    CHECK(result.receipt == nullptr);
}

void check_rejected_gate(const GateFlags& flags, const std::string& expected) {
    const auto state = empty_state();
    const auto candidate = proposal(state);
    const auto result = bounded_verification_write(
        state, candidate, gates(candidate, "accept", flags),
        BoundedWorldWriteConfig(), true);
    CHECK(result.reason == expected);
    CHECK(result.state == state);
    CHECK(!result.authorized);
    CHECK(!result.committed);
    CHECK(result.receipt == nullptr);
    CHECK(cognitive_state_hash(*result.state) == cognitive_state_hash(*state));
}

void check_gate_precedence_and_same_identity_rejections() {
    {
        const auto state = empty_state();
        const auto candidate = proposal(state);
        const auto result = bounded_verification_write(
            state, candidate, gates(candidate, "abstain"),
            BoundedWorldWriteConfig(), true);
        CHECK(result.reason == "evidence_not_accepted");
        CHECK(result.state == state);
        CHECK(!result.authorized && !result.committed && result.receipt == nullptr);
    }
    GateFlags flags;
    flags.definitions_complete = false;
    check_rejected_gate(flags, "definitions_incomplete");
    flags = {};
    flags.regime_change_suspected = true;
    check_rejected_gate(flags, "regime_change_suspected");
    flags = {};
    flags.device_gate_passed = false;
    check_rejected_gate(flags, "device_gate");
    flags = {};
    flags.evidence_current = false;
    check_rejected_gate(flags, "evidence_expired");
    flags = {};
    flags.accumulator_revision_current = false;
    check_rejected_gate(flags, "accumulator_revision_stale");
    flags = {};
    flags.runtime_context_safe = false;
    check_rejected_gate(flags, "runtime_context_unsafe");

    const auto state = empty_state();
    const auto candidate = proposal(state);
    const auto strict = bounded_verification_write(
        state, candidate, gates(candidate),
        BoundedWorldWriteConfig(0.99), true);
    CHECK(strict.reason == "causal_lower_bound");
    CHECK(strict.state == state);
}

void check_plain_modified_and_foreign_capabilities_rejected() {
    const auto state = empty_state();
    const auto candidate = proposal(state);
    const WorldWriteGates plain(
        "accept", 1.0, 1'000, 1'000, true, true, true, false,
        true, true, true, true, true, true);
    check_throws<AuthorityError>([&] {
        static_cast<void>(bounded_verification_write(
            state, candidate, plain, BoundedWorldWriteConfig(), true));
    });

    const auto minted = gates(candidate);
    const WorldWriteGates modified(
        minted.evidence_status, 1.0, minted.source_diversity,
        minted.context_diversity, minted.definitions_complete,
        minted.counterfactual_support, minted.intervention_support,
        minted.regime_change_suspected, minted.slot_gate_passed,
        minted.device_gate_passed, minted.capacity_strategy_safe,
        minted.evidence_current, minted.accumulator_revision_current,
        minted.runtime_context_safe);
    check_throws<AuthorityError>([&] {
        static_cast<void>(bounded_verification_write(
            state, candidate, modified, BoundedWorldWriteConfig(), true));
    });

    const auto accepted = decision();
    const AccumulatorDecision foreign(
        accepted->status, accepted->reason, accepted->posterior_mean,
        accepted->causal_lower_bound, accepted->overall_upper_bound,
        accepted->effective_sample_size, accepted->source_diversity,
        accepted->context_diversity, accepted->regime_change_score,
        accepted->revision, accepted->hypothesis_id, accepted->evidence_addresses);
    const EvidenceRevisionVerification foreign_revision(
        true, true, {}, {}, {}, {}, std::nullopt, "world-write-test", {}, {}, {
            {"status", foreign.status}, {"reason", foreign.reason},
            {"posterior_mean", foreign.posterior_mean},
            {"causal_lower_bound", foreign.causal_lower_bound},
            {"overall_upper_bound", foreign.overall_upper_bound},
            {"effective_sample_size", foreign.effective_sample_size},
            {"source_diversity", static_cast<std::int64_t>(foreign.source_diversity)},
            {"context_diversity", static_cast<std::int64_t>(foreign.context_diversity)},
            {"regime_change_score", foreign.regime_change_score},
            {"revision", static_cast<std::int64_t>(foreign.revision)},
            {"hypothesis_id", foreign.hypothesis_id},
            {"evidence_addresses", foreign.evidence_addresses},
        });
    check_throws<AuthorityError>([&] {
        static_cast<void>(world_write_gates_from_decision(
            foreign, candidate, true, true, true, false, true, true, true,
            true, true, true, &foreign_revision));
    });
}

void check_weight_and_target_rejections() {
    const auto state = empty_state();
    const auto low_weight = proposal(
        state, std::string("verification"),
        Tensor(TensorDType::float32, {1, 2}, {0.0, 0.0}));
    const auto rejected = bounded_verification_write(
        state, low_weight, gates(low_weight), BoundedWorldWriteConfig(), true);
    CHECK(rejected.reason == "proposal_weight");
    CHECK(rejected.state == state);

    const auto global = proposal(state, std::string("global"));
    const auto global_gates = gates(global);
    check_throws<std::invalid_argument>([&] {
        static_cast<void>(bounded_verification_write(
            state, global, global_gates, BoundedWorldWriteConfig(), true));
    });
}

}  // namespace

int main() {
    check_authority_binds_hypothesis_evidence_and_proposal();
    check_bounded_commit_receipt_and_partition();
    check_receipt_roundtrip_and_exact_rollback();
    check_scoped_retraction_preserves_unrelated_cognition();
    check_scoped_retraction_requires_lifo_and_restores_prior_head();
    check_retraction_rejects_unrelated_slot_and_tampered_receipt();
    check_malformed_topologies_fail_before_partition_indexing();
    check_receipt_numeric_boundaries_and_python_scalar_coercion();
    check_lifo_revision_distinguishes_values_above_double_integer_precision();
    check_revision_verification_payload_large_integer_exactness();
    check_cpp_proposal_vector_hash_profile();
    check_dry_run_preserves_shared_identity();
    check_gate_precedence_and_same_identity_rejections();
    check_plain_modified_and_foreign_capabilities_rejected();
    check_weight_and_target_rejections();
    std::cout << "PASS bounded write authority, receipt, rollback, and scoped retraction\n";
}
