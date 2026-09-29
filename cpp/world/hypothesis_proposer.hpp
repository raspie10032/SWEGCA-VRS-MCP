#pragma once

#include "world/cognitive_event.hpp"
#include "world/evidence_accumulator.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace swegca::world {

class HypothesisProposerConfig final {
public:
    HypothesisProposerConfig(
        double minimum_claim_confidence = 0.5,
        std::uint64_t minimum_source_diversity = 2,
        double conflict_score_multiplier = 0.5,
        std::size_t maximum_candidates = 8,
        std::size_t maximum_requested_axes = 2,
        std::vector<std::string> ignored_predicates = {
            "entity_binding_status", "candidate_entities"},
        std::vector<std::pair<std::string, std::string>> axis_request_kinds = {
            {"observational", "independent_observation"},
            {"counterfactual", "counterfactual_observation"},
            {"intervention", "reversible_intervention"},
            {"cross_context", "new_context_observation"}});

    const double minimum_claim_confidence;
    const std::uint64_t minimum_source_diversity;
    const double conflict_score_multiplier;
    const std::size_t maximum_candidates;
    const std::size_t maximum_requested_axes;
    const std::vector<std::string> ignored_predicates;
    const std::vector<std::pair<std::string, std::string>> axis_request_kinds;
};

struct HypothesisCandidate final {
    std::string hypothesis_id;
    std::optional<std::string> subject;
    std::string predicate;
    JsonValue value;
    double confidence = 0.0;
    std::vector<std::string> evidence_refs;
    std::vector<std::string> source_families;
    std::vector<std::string> source_event_ids;
    std::vector<JsonValue> conflicting_values;

    [[nodiscard]] bool has_conflict() const noexcept {
        return !conflicting_values.empty();
    }
};

struct EvidenceRequestPlan final {
    std::string hypothesis_id;
    std::vector<std::string> requested_axes;
    std::vector<std::string> request_kinds;
    std::string reason;
};

[[nodiscard]] std::vector<HypothesisCandidate> propose_hypotheses(
    std::span<const CognitiveEvent> events,
    const HypothesisProposerConfig& config = {});

[[nodiscard]] EvidenceRequestPlan plan_evidence_request(
    const HypothesisCandidate& candidate,
    const EvidenceAccumulatorState* accumulator,
    const EvidenceAccumulatorConfig& accumulator_config,
    const HypothesisProposerConfig& proposer_config,
    bool external_refutation = false);

[[nodiscard]] constexpr std::string_view hypothesis_proposer_source_sha256() noexcept {
    return "5b4d0bd0c5f086ea30240d9d70dc868f076c40bd68377ddbfa340df6261c199d";
}

}  // namespace swegca::world
