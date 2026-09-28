#include "world/bounded_world_write.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
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
    check_dry_run_preserves_shared_identity();
    check_gate_precedence_and_same_identity_rejections();
    check_plain_modified_and_foreign_capabilities_rejected();
    check_weight_and_target_rejections();
    std::cout << "PASS bounded verification write authority, gates, digest, and receipt\n";
}
