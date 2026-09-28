#include "world/re_evidence_transaction.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace swegca::world {
namespace {

using ProvenanceKey = std::pair<std::string, std::string>;

struct ObservationGroup final {
    ProvenanceKey key;
    std::vector<const EvidenceObservation*> observations;
};

[[nodiscard]] double dtype_epsilon(const TensorDType dtype) noexcept {
    switch (dtype) {
    case TensorDType::float64:
        return std::numeric_limits<double>::epsilon();
    case TensorDType::float32:
        return static_cast<double>(std::numeric_limits<float>::epsilon());
    case TensorDType::float16:
        return 0x1p-10;
    case TensorDType::bfloat16:
        return 0x1p-7;
    }
    return std::numeric_limits<double>::epsilon();
}

[[nodiscard]] std::set<std::string, std::less<>> address_set(
    const std::span<const std::string> addresses) {
    return {addresses.begin(), addresses.end()};
}

[[nodiscard]] std::vector<const CurrentEvidenceDisposition*>
eligible_current_evidence(const MainReEvidenceReceipt& receipt) {
    if (receipt.verdict != "support" && receipt.verdict != "refute") return {};
    if (receipt.should_abstain || !receipt.semantic_judgment_formed)
        throw std::invalid_argument(
            "abstaining Re-evidence cannot enter the accumulator");
    if (receipt.current_evidence.empty() ||
        std::any_of(receipt.current_evidence.begin(), receipt.current_evidence.end(),
                    [](const auto& item) { return !item.transaction_ready; }))
        throw std::invalid_argument("current evidence provenance is incomplete");

    std::vector<const CandidateDisposition*> selected;
    for (const auto& candidate : receipt.candidates)
        if (candidate.selected) selected.push_back(&candidate);
    if (selected.size() != 1 ||
        selected.front()->source_address_status != "present" ||
        selected.front()->revision_status != "present")
        throw std::invalid_argument(
            "selected replay candidate provenance is incomplete");
    const auto& candidate = *selected.front();
    for (const auto& item : receipt.current_evidence) {
        if (!item.source_address || !item.source_revision ||
            std::find(candidate.source_addresses.begin(),
                      candidate.source_addresses.end(), *item.source_address) ==
                candidate.source_addresses.end() ||
            item.source_revision != candidate.revision)
            throw std::invalid_argument(
                "selected replay candidate is not exactly bound to current evidence");
    }

    std::vector<const CurrentEvidenceDisposition*> eligible;
    bool opposed = false;
    for (const auto& item : receipt.current_evidence) {
        if (item.verification_outcome &&
            *item.verification_outcome == receipt.verdict)
            eligible.push_back(&item);
        if (item.verification_outcome &&
            (*item.verification_outcome == "support" ||
             *item.verification_outcome == "refute") &&
            *item.verification_outcome != receipt.verdict)
            opposed = true;
    }
    if (eligible.empty() || opposed)
        throw std::invalid_argument(
            "current evidence does not support the main verdict");
    return eligible;
}

[[nodiscard]] JsonValue optional_string(
    const std::optional<std::string>& value) {
    return value ? JsonValue(*value) : JsonValue(nullptr);
}

[[nodiscard]] JsonValue optional_integer(
    const std::optional<std::int64_t>& value) {
    return value ? JsonValue(*value) : JsonValue(nullptr);
}

[[nodiscard]] JsonValue optional_number(const std::optional<double>& value) {
    return value ? JsonValue(*value) : JsonValue(nullptr);
}

[[nodiscard]] JsonValue string_array(
    const std::span<const std::string> values) {
    JsonValue::Array result;
    result.reserve(values.size());
    for (const auto& value : values) result.emplace_back(value);
    return result;
}

[[nodiscard]] CognitiveEvent current_evidence_event(
    const MainReEvidenceReceipt& receipt,
    const std::span<const CurrentEvidenceDisposition* const> evidence,
    std::string event_id) {
    if (!receipt.proposition)
        throw std::invalid_argument(
            "semantic Re-evidence judgment lacks a proposition");
    std::vector<std::string> source_refs;
    std::unordered_set<std::string> seen;
    JsonValue::Array bindings;
    bindings.reserve(evidence.size());
    for (const auto* item : evidence) {
        if (seen.insert(item->evidence_ref).second)
            source_refs.push_back(item->evidence_ref);
        if (item->source_address && seen.insert(*item->source_address).second)
            source_refs.push_back(*item->source_address);
        bindings.emplace_back(JsonValue::Object{
            {"evidence_ref", item->evidence_ref},
            {"source_address", optional_string(item->source_address)},
            {"source_revision", optional_string(item->source_revision)},
            {"source_family", optional_string(item->source_family)},
            {"context_hash", optional_string(item->context_hash)},
            {"axis", optional_string(item->axis)},
            {"verification_outcome", optional_string(item->verification_outcome)},
            {"observed_at", optional_integer(item->observed_at)},
            {"expires_at", optional_integer(item->expires_at)},
            {"producer_id", optional_string(item->producer_id)},
            {"producer_confidence", optional_number(item->producer_confidence)}});
    }
    std::vector<std::string> proposal_addresses;
    proposal_addresses.reserve(receipt.proposals.size());
    for (const auto& proposal : receipt.proposals)
        proposal_addresses.push_back(proposal.source_address);

    return CognitiveEvent(
        std::move(event_id), "re_evidence_current_observation",
        EventSource("re_evidence_current_observation_bundle_v1",
                    "sole_main_re_evidence_to_accumulator_v1",
                    *evidence.front()->source_address),
        {EvidenceClaim(
            "re_evidence_relation",
            JsonValue::Object{{"proposition", *receipt.proposition},
                              {"verdict", receipt.verdict}},
            1.0, receipt.selected_episode_id)},
        std::move(source_refs), EvidenceKind::observed_evidence,
        JsonValue::Object{
            {"request_sha256", receipt.request_sha256},
            {"selected_episode_id", optional_string(receipt.selected_episode_id)},
            {"current_evidence_bindings", std::move(bindings)},
            {"replay_evidence_refs_excluded",
             string_array(receipt.replay_evidence_refs)},
            {"proposal_addresses_excluded", string_array(proposal_addresses)},
            {"world_write_authority", false}});
}

template <typename State>
[[nodiscard]] SynapseProposal make_experience_proposal(
    const std::shared_ptr<const State>& world,
    const AccumulatorDecision& decision,
    const std::span<const EvidenceObservation> observations,
    const std::span<const ExperienceVectorBinding> vectors,
    std::string source) {
    require_authoritative_accumulator_decision(decision);
    if (decision.status != "accept") {
        throw AuthorityError("experience delta requires accumulator acceptance");
    }

    const std::set<std::string, std::less<>> accepted(
        decision.evidence_addresses.begin(), decision.evidence_addresses.end());
    std::vector<std::string> observed_addresses;
    observed_addresses.reserve(observations.size());
    for (const auto& observation : observations) {
        observed_addresses.push_back(observation.evidence_address);
    }
    const std::set<std::string, std::less<>> observed(
        observed_addresses.begin(), observed_addresses.end());
    if (accepted.empty() || observed_addresses.size() != observed.size() ||
        accepted != observed) {
        throw std::invalid_argument(
            "accepted and admitted evidence sets differ");
    }
    for (const auto& observation : observations) {
        if (observation.source_address.empty() ||
            observation.source_revision.empty() ||
            (observation.outcome != "support" &&
             observation.outcome != "refute")) {
            throw std::invalid_argument(
                "experience delta observations lack verified provenance");
        }
    }

    std::vector<ObservationGroup> groups;
    for (const auto& observation : observations) {
        const ProvenanceKey key{observation.source_address,
                                observation.source_revision};
        const auto found = std::find_if(
            groups.begin(), groups.end(),
            [&](const ObservationGroup& group) { return group.key == key; });
        if (found == groups.end()) {
            groups.push_back({key, {&observation}});
        } else {
            found->observations.push_back(&observation);
        }
    }

    std::set<ProvenanceKey> binding_keys;
    bool duplicate_binding = false;
    for (const auto& binding : vectors) {
        duplicate_binding |= !binding_keys
                                  .insert({binding.source_address,
                                           binding.source_revision})
                                  .second;
    }
    std::set<ProvenanceKey> grouped_keys;
    for (const auto& group : groups) grouped_keys.insert(group.key);
    if (duplicate_binding || binding_keys != grouped_keys) {
        throw std::invalid_argument(
            "experience vector source/revision bindings differ");
    }

    const Tensor slots = state_slot_tensor(world);
    const auto dimension = slots.shape().back();
    std::vector<std::vector<double>> signed_vectors;
    signed_vectors.reserve(groups.size());
    for (const auto& group : groups) {
        const auto binding = std::find_if(
            vectors.begin(), vectors.end(), [&](const ExperienceVectorBinding& item) {
                return item.source_address == group.key.first &&
                       item.source_revision == group.key.second;
            });
        std::set<std::string, std::less<>> group_addresses;
        std::set<std::string, std::less<>> outcomes;
        for (const auto* observation : group.observations) {
            group_addresses.insert(observation->evidence_address);
            outcomes.insert(observation->outcome);
        }
        if (address_set(binding->evidence_addresses) != group_addresses) {
            throw std::invalid_argument(
                "experience vector evidence binding differs");
        }
        if (outcomes.size() != 1) {
            throw std::invalid_argument(
                "one source revision has conflicting current outcomes");
        }
        if (binding->values.size() != dimension) {
            throw std::invalid_argument(
                "experience vector dimension differs from World state");
        }
        const Tensor stored(slots.dtype(), {dimension}, binding->values,
                            std::string(slots.device()));
        std::vector<double> values(stored.values().begin(), stored.values().end());
        if (*outcomes.begin() == "refute") {
            for (double& value : values) value = -value;
        }
        signed_vectors.push_back(std::move(values));
    }

    std::vector<double> mean(static_cast<std::size_t>(dimension), 0.0);
    for (const auto& values : signed_vectors) {
        for (std::size_t index = 0; index < mean.size(); ++index) {
            mean[index] += values[index];
        }
    }
    for (double& value : mean) {
        value /= static_cast<double>(signed_vectors.size());
    }
    const Tensor evidence_delta(slots.dtype(), {1, dimension}, std::move(mean),
                                std::string(slots.device()));
    double absolute_sum = 0.0;
    for (const double value : evidence_delta.values()) {
        if (!std::isfinite(value)) {
            throw std::invalid_argument(
                "accepted experience produced no finite directional delta");
        }
        absolute_sum += std::abs(value);
    }
    if (!(absolute_sum > 0.0)) {
        throw std::invalid_argument(
            "accepted experience produced no finite directional delta");
    }

    const double epsilon = dtype_epsilon(evidence_delta.dtype());
    const double probability = std::min(
        std::max(decision.posterior_mean, epsilon), 1.0 - epsilon);
    const Tensor evidence_logits(
        evidence_delta.dtype(), {1, 2},
        {std::log(probability), std::log1p(-probability)},
        std::string(evidence_delta.device()));
    return evidence_delta_proposal(
        world, evidence_delta, evidence_logits, std::move(source),
        {decision.evidence_addresses}, decision.hypothesis_id,
        std::string("verification"));
}

}  // namespace

ExperienceVectorBinding::ExperienceVectorBinding(
    std::string source_address_value, std::string source_revision_value,
    std::vector<std::string> evidence_addresses_value,
    std::vector<double> values_value)
    : source_address(std::move(source_address_value)),
      source_revision(std::move(source_revision_value)),
      evidence_addresses(std::move(evidence_addresses_value)),
      values(std::move(values_value)) {
    if (source_address.empty() || source_revision.empty()) {
        throw std::invalid_argument(
            "experience vector source provenance is incomplete");
    }
    if (evidence_addresses.empty() ||
        address_set(evidence_addresses).size() != evidence_addresses.size()) {
        throw std::invalid_argument(
            "experience vector evidence addresses are invalid");
    }
    if (values.empty() ||
        !std::all_of(values.begin(), values.end(),
                     [](const double value) { return std::isfinite(value); })) {
        throw std::invalid_argument(
            "experience vector values must be finite and nonempty");
    }
    if (std::none_of(values.begin(), values.end(),
                     [](const double value) { return value != 0.0; })) {
        throw std::invalid_argument("experience vector must be nonzero");
    }
}

ReEvidenceAccumulatorAdmission::ReEvidenceAccumulatorAdmission(
    std::shared_ptr<const EvidenceAccumulatorState> state_value,
    std::optional<CognitiveEvent> event_value,
    std::vector<EvidenceObservation> observations_value,
    std::vector<AccumulatorUpdate> updates_value,
    std::shared_ptr<const AccumulatorDecision> decision_value,
    const bool admitted_value, std::string reason_value,
    const bool world_write_eligible_value)
    : state(std::move(state_value)), event(std::move(event_value)),
      observations(std::move(observations_value)),
      updates(std::move(updates_value)), decision(std::move(decision_value)),
      admitted(admitted_value), reason(std::move(reason_value)),
      world_write_eligible(world_write_eligible_value) {
    if (!state) throw std::invalid_argument("accumulator state is null");
    if (world_write_eligible !=
        (admitted && decision && decision->status == "accept"))
        throw std::invalid_argument("Re-evidence World-write eligibility changed");
    if (!admitted &&
        (event.has_value() || !observations.empty() || !updates.empty() || decision))
        throw std::invalid_argument(
            "non-admitted Re-evidence produced accumulator authority");
}

SynapseProposal experience_delta_proposal_from_accepted_observations(
    std::shared_ptr<const WorldState> world,
    const AccumulatorDecision& decision,
    const std::span<const EvidenceObservation> observations,
    const std::span<const ExperienceVectorBinding> vectors,
    std::string source) {
    return make_experience_proposal(std::move(world), decision, observations,
                                    vectors, std::move(source));
}

SynapseProposal experience_delta_proposal_from_accepted_observations(
    std::shared_ptr<const CognitiveState> world,
    const AccumulatorDecision& decision,
    const std::span<const EvidenceObservation> observations,
    const std::span<const ExperienceVectorBinding> vectors,
    std::string source) {
    return make_experience_proposal(std::move(world), decision, observations,
                                    vectors, std::move(source));
}

ReEvidenceAccumulatorAdmission admit_re_evidence_to_accumulator(
    const MainReEvidenceReceipt& receipt,
    std::shared_ptr<const EvidenceAccumulatorState> state,
    const EvidenceAccumulatorConfig& config, std::string hypothesis_id,
    std::string event_id, const std::int64_t current_step) {
    if (!state) throw std::invalid_argument("accumulator state is null");
    if (state->hypothesis_id() != hypothesis_id)
        throw std::invalid_argument("Re-evidence and accumulator hypothesis differ");
    if (current_step < 0)
        throw std::invalid_argument("current_step must be nonnegative");
    if (receipt.verdict == "conflict" || receipt.verdict == "insufficient")
        return ReEvidenceAccumulatorAdmission(
            std::move(state), std::nullopt, {}, {}, nullptr, false,
            "main_" + receipt.verdict + "_abstention", false);

    const auto eligible = eligible_current_evidence(receipt);
    std::vector<EvidenceObservation> observations;
    observations.reserve(eligible.size());
    for (const auto* item : eligible) {
        if (!item->source_address || !item->source_revision ||
            !item->source_family || !item->context_hash || !item->axis ||
            !item->verification_outcome || !item->observed_at ||
            !item->producer_id || !item->producer_confidence)
            throw std::invalid_argument("current evidence provenance is incomplete");
        observations.emplace_back(
            hypothesis_id, item->evidence_ref, *item->source_family,
            *item->context_hash, *item->axis, *item->verification_outcome,
            *item->observed_at, item->expires_at, *item->producer_id,
            *item->producer_confidence, *item->source_address,
            *item->source_revision);
    }
    for (const auto& observation : observations) {
        if (std::find(config.required_axes.begin(), config.required_axes.end(),
                      observation.axis) == config.required_axes.end())
            throw std::invalid_argument(
                "current evidence axis is not registered by the accumulator");
    }
    for (const auto& observation : observations) {
        if (observation.expires_at && current_step > *observation.expires_at)
            throw std::invalid_argument(
                "current Re-evidence observation expired before admission");
    }
    for (const auto& observation : observations) {
        if (std::find(state->seen_addresses().begin(), state->seen_addresses().end(),
                      observation.evidence_address) != state->seen_addresses().end())
            throw std::invalid_argument(
                "current Re-evidence observation was already accumulated");
    }

    auto event = current_evidence_event(receipt, eligible, std::move(event_id));
    auto updated = state;
    std::vector<AccumulatorUpdate> updates;
    updates.reserve(observations.size());
    for (const auto& observation : observations) {
        auto update = update_accumulator(updated, observation, config, current_step);
        if (!update.applied)
            throw std::runtime_error(
                "preflighted Re-evidence observation was not applied");
        updated = update.state;
        updates.push_back(std::move(update));
    }
    auto decision = assess_accumulator(*updated, config);
    const bool eligible_for_write = decision->status == "accept";
    const std::string reason = "accumulator_" + decision->status;
    return ReEvidenceAccumulatorAdmission(
        std::move(updated), std::move(event), std::move(observations),
        std::move(updates), std::move(decision), true, reason,
        eligible_for_write);
}

}  // namespace swegca::world
