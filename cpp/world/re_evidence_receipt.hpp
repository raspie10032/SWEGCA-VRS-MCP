#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace swegca::world {

class ProposalDisposition final {
public:
    ProposalDisposition(std::string source, std::string source_address,
                        bool accepted,
                        std::optional<std::string> rejection_reason);

    const std::string source;
    const std::string source_address;
    const bool accepted;
    const std::optional<std::string> rejection_reason;

    friend bool operator==(const ProposalDisposition&,
                           const ProposalDisposition&) = default;
};

class CandidateDisposition final {
public:
    CandidateDisposition(
        std::string episode_id, bool selected,
        std::vector<std::string> evidence_refs,
        std::vector<std::string> source_addresses,
        std::string source_address_status, std::string verification_state,
        std::optional<std::string> revision, std::string revision_status,
        std::optional<std::string> rejection_reason);

    const std::string episode_id;
    const bool selected;
    const std::vector<std::string> evidence_refs;
    const std::vector<std::string> source_addresses;
    const std::string source_address_status;
    const std::string verification_state;
    const std::optional<std::string> revision;
    const std::string revision_status;
    const std::optional<std::string> rejection_reason;

    friend bool operator==(const CandidateDisposition&,
                           const CandidateDisposition&) = default;
};

class CurrentEvidenceDisposition final {
public:
    CurrentEvidenceDisposition(
        std::string evidence_ref, std::string observation,
        std::string source_kind, std::optional<std::string> source_address,
        std::optional<std::string> source_revision,
        std::optional<std::string> source_family,
        std::optional<std::string> context_hash, std::optional<std::string> axis,
        std::optional<std::string> verification_outcome,
        std::optional<std::int64_t> observed_at,
        std::optional<std::int64_t> expires_at,
        std::optional<std::string> producer_id,
        std::optional<double> producer_confidence, bool transaction_ready,
        std::string revision_status);

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
    const bool transaction_ready;
    const std::string revision_status;

    friend bool operator==(const CurrentEvidenceDisposition&,
                           const CurrentEvidenceDisposition&) = default;
};

class MainReEvidenceReceipt final {
public:
    MainReEvidenceReceipt(
        std::string schema_version, std::string state_owner,
        std::string request_sha256, std::string user_query,
        std::optional<std::string> selected_episode_id,
        std::optional<std::string> proposition, std::string verdict,
        std::string rationale, std::vector<std::string> current_evidence_refs,
        std::vector<CurrentEvidenceDisposition> current_evidence,
        std::vector<std::string> replay_evidence_refs,
        std::vector<CandidateDisposition> candidates,
        std::vector<ProposalDisposition> proposals,
        std::vector<std::string> executed_cores, bool fanout_used,
        std::int64_t elapsed_ns, bool unresolved_conflict,
        bool insufficient_evidence, bool should_abstain,
        bool semantic_judgment_formed, std::int64_t persistent_state_count = 1,
        bool persistent_state_mutated = false, bool manager_retained = false,
        bool worker_state_retained = false, bool action_authorized = false,
        bool persistent_write_authorized = false,
        bool semantic_promotion_authorized = false,
        bool world_write_authorized = false, bool model_update_authorized = false,
        bool distribution_authorized = false, bool p3_authorized = false);

    const std::string schema_version;
    const std::string state_owner;
    const std::string request_sha256;
    const std::string user_query;
    const std::optional<std::string> selected_episode_id;
    const std::optional<std::string> proposition;
    const std::string verdict;
    const std::string rationale;
    const std::vector<std::string> current_evidence_refs;
    const std::vector<CurrentEvidenceDisposition> current_evidence;
    const std::vector<std::string> replay_evidence_refs;
    const std::vector<CandidateDisposition> candidates;
    const std::vector<ProposalDisposition> proposals;
    const std::vector<std::string> executed_cores;
    const bool fanout_used;
    const std::int64_t elapsed_ns;
    const bool unresolved_conflict;
    const bool insufficient_evidence;
    const bool should_abstain;
    const bool semantic_judgment_formed;
    const std::int64_t persistent_state_count;
    const bool persistent_state_mutated;
    const bool manager_retained;
    const bool worker_state_retained;
    const bool action_authorized;
    const bool persistent_write_authorized;
    const bool semantic_promotion_authorized;
    const bool world_write_authorized;
    const bool model_update_authorized;
    const bool distribution_authorized;
    const bool p3_authorized;
};

}  // namespace swegca::world
