#pragma once

#include "world/re_evidence_receipt.hpp"
#include "world/world_state.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace swegca::world {

class ReEvidenceCandidate final {
public:
    ReEvidenceCandidate(
        std::string episode_id, std::vector<std::string> matched_cues,
        std::vector<std::string> propositions,
        std::vector<std::string> historical_outcomes,
        std::vector<std::string> evidence_refs,
        std::vector<std::string> source_addresses,
        std::string verification_state, std::optional<std::string> revision);

    const std::string episode_id;
    const std::vector<std::string> matched_cues;
    const std::vector<std::string> propositions;
    const std::vector<std::string> historical_outcomes;
    const std::vector<std::string> evidence_refs;
    const std::vector<std::string> source_addresses;
    const std::string verification_state;
    const std::optional<std::string> revision;

    friend bool operator==(const ReEvidenceCandidate&,
                           const ReEvidenceCandidate&) = default;
};

class ReEvidenceCurrentEvidence final {
public:
    ReEvidenceCurrentEvidence(
        std::string evidence_ref, std::string observation,
        std::string source_kind,
        std::optional<std::string> source_address = std::nullopt,
        std::optional<std::string> source_revision = std::nullopt,
        std::optional<std::string> source_family = std::nullopt,
        std::optional<std::string> context_hash = std::nullopt,
        std::optional<std::string> axis = std::nullopt,
        std::optional<std::string> verification_outcome = std::nullopt,
        std::optional<std::int64_t> observed_at = std::nullopt,
        std::optional<std::int64_t> expires_at = std::nullopt,
        std::optional<std::string> producer_id = std::nullopt,
        std::optional<double> producer_confidence = std::nullopt);

    [[nodiscard]] bool transaction_ready() const noexcept;

    const std::string evidence_ref;
    const std::string observation;
    const std::string source_kind;
    const std::optional<std::string> source_address;
    const std::optional<std::string> source_revision;
    const std::optional<std::string> source_family;
    const std::optional<std::string> context_hash;
    const std::optional<std::string> axis;
    const std::optional<std::string> verification_outcome;
    const std::optional<std::int64_t> observed_at;
    const std::optional<std::int64_t> expires_at;
    const std::optional<std::string> producer_id;
    const std::optional<double> producer_confidence;

    friend bool operator==(const ReEvidenceCurrentEvidence&,
                           const ReEvidenceCurrentEvidence&) = default;
};

class PreparedReEvidenceRequest final {
public:
    PreparedReEvidenceRequest(
        std::string request_sha256, std::string user_query,
        std::vector<ReEvidenceCandidate> candidates,
        std::vector<ReEvidenceCurrentEvidence> current_evidence,
        std::optional<std::string> current_evidence_target_episode_id);

    [[nodiscard]] const ReEvidenceCandidate* candidate(
        std::string_view episode_id) const noexcept;
    [[nodiscard]] std::vector<std::string> current_evidence_refs() const;
    [[nodiscard]] std::vector<std::string> current_observations() const;
    [[nodiscard]] std::vector<std::string> current_source_kinds() const;
    [[nodiscard]] std::vector<std::string> current_source_addresses() const;

    const std::string request_sha256;
    const std::string user_query;
    const std::vector<ReEvidenceCandidate> candidates;
    const std::vector<ReEvidenceCurrentEvidence> current_evidence;
    const std::optional<std::string> current_evidence_target_episode_id;
};

class ReEvidenceProposal final {
public:
    ReEvidenceProposal(
        std::string source, std::string source_address,
        std::string request_sha256,
        std::optional<std::string> selected_episode_id,
        std::string proposition, std::string verdict, std::string rationale,
        bool action_authorized = false,
        bool persistent_write_authorized = false,
        bool semantic_promotion_authorized = false);

    const std::string source;
    const std::string source_address;
    const std::string request_sha256;
    const std::optional<std::string> selected_episode_id;
    const std::string proposition;
    const std::string verdict;
    const std::string rationale;
    const bool action_authorized;
    const bool persistent_write_authorized;
    const bool semantic_promotion_authorized;
};

[[nodiscard]] PreparedReEvidenceRequest prepare_re_evidence_request(
    std::string serialized_input, std::string expected_sha256);

template <typename State>
struct MainReEvidenceResult final {
    std::shared_ptr<const State> state;
    MainReEvidenceReceipt receipt;
};

template <typename State>
using ReEvidenceCore = std::function<ReEvidenceProposal(
    State& transient_state, const PreparedReEvidenceRequest& request)>;

using WorldReEvidenceCores =
    std::map<std::string, ReEvidenceCore<WorldState>, std::less<>>;
using CognitiveReEvidenceCores =
    std::map<std::string, ReEvidenceCore<CognitiveState>, std::less<>>;

[[nodiscard]] MainReEvidenceResult<WorldState> run_re_evidence_manager(
    std::shared_ptr<const WorldState> state,
    const PreparedReEvidenceRequest& request,
    const WorldReEvidenceCores& resident_cores,
    std::span<const std::string> selected_cores);

[[nodiscard]] MainReEvidenceResult<CognitiveState> run_re_evidence_manager(
    std::shared_ptr<const CognitiveState> state,
    const PreparedReEvidenceRequest& request,
    const CognitiveReEvidenceCores& resident_cores,
    std::span<const std::string> selected_cores);

}  // namespace swegca::world
