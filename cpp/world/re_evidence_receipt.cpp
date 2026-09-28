#include "world/re_evidence_receipt.hpp"

#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace swegca::world {

ProposalDisposition::ProposalDisposition(
    std::string source_value, std::string source_address_value,
    const bool accepted_value,
    std::optional<std::string> rejection_reason_value)
    : source(std::move(source_value)),
      source_address(std::move(source_address_value)), accepted(accepted_value),
      rejection_reason(std::move(rejection_reason_value)) {}

CandidateDisposition::CandidateDisposition(
    std::string episode_id_value, const bool selected_value,
    std::vector<std::string> evidence_refs_value,
    std::vector<std::string> source_addresses_value,
    std::string source_address_status_value,
    std::string verification_state_value,
    std::optional<std::string> revision_value,
    std::string revision_status_value,
    std::optional<std::string> rejection_reason_value)
    : episode_id(std::move(episode_id_value)), selected(selected_value),
      evidence_refs(std::move(evidence_refs_value)),
      source_addresses(std::move(source_addresses_value)),
      source_address_status(std::move(source_address_status_value)),
      verification_state(std::move(verification_state_value)),
      revision(std::move(revision_value)),
      revision_status(std::move(revision_status_value)),
      rejection_reason(std::move(rejection_reason_value)) {}

CurrentEvidenceDisposition::CurrentEvidenceDisposition(
    std::string evidence_ref_value, std::string observation_value,
    std::string source_kind_value,
    std::optional<std::string> source_address_value,
    std::optional<std::string> source_revision_value,
    std::optional<std::string> source_family_value,
    std::optional<std::string> context_hash_value,
    std::optional<std::string> axis_value,
    std::optional<std::string> verification_outcome_value,
    std::optional<std::int64_t> observed_at_value,
    std::optional<std::int64_t> expires_at_value,
    std::optional<std::string> producer_id_value,
    std::optional<double> producer_confidence_value,
    const bool transaction_ready_value, std::string revision_status_value)
    : evidence_ref(std::move(evidence_ref_value)),
      observation(std::move(observation_value)),
      source_kind(std::move(source_kind_value)),
      source_address(std::move(source_address_value)),
      source_revision(std::move(source_revision_value)),
      source_family(std::move(source_family_value)),
      context_hash(std::move(context_hash_value)), axis(std::move(axis_value)),
      verification_outcome(std::move(verification_outcome_value)),
      observed_at(observed_at_value), expires_at(expires_at_value),
      producer_id(std::move(producer_id_value)),
      producer_confidence(producer_confidence_value),
      transaction_ready(transaction_ready_value),
      revision_status(std::move(revision_status_value)) {}

MainReEvidenceReceipt::MainReEvidenceReceipt(
    std::string schema_version_value, std::string state_owner_value,
    std::string request_sha256_value, std::string user_query_value,
    std::optional<std::string> selected_episode_id_value,
    std::optional<std::string> proposition_value, std::string verdict_value,
    std::string rationale_value, std::vector<std::string> current_evidence_refs_value,
    std::vector<CurrentEvidenceDisposition> current_evidence_value,
    std::vector<std::string> replay_evidence_refs_value,
    std::vector<CandidateDisposition> candidates_value,
    std::vector<ProposalDisposition> proposals_value,
    std::vector<std::string> executed_cores_value, const bool fanout_used_value,
    const std::int64_t elapsed_ns_value, const bool unresolved_conflict_value,
    const bool insufficient_evidence_value, const bool should_abstain_value,
    const bool semantic_judgment_formed_value,
    const std::int64_t persistent_state_count_value,
    const bool persistent_state_mutated_value, const bool manager_retained_value,
    const bool worker_state_retained_value, const bool action_authorized_value,
    const bool persistent_write_authorized_value,
    const bool semantic_promotion_authorized_value,
    const bool world_write_authorized_value, const bool model_update_authorized_value,
    const bool distribution_authorized_value, const bool p3_authorized_value)
    : schema_version(std::move(schema_version_value)),
      state_owner(std::move(state_owner_value)),
      request_sha256(std::move(request_sha256_value)),
      user_query(std::move(user_query_value)),
      selected_episode_id(std::move(selected_episode_id_value)),
      proposition(std::move(proposition_value)), verdict(std::move(verdict_value)),
      rationale(std::move(rationale_value)),
      current_evidence_refs(std::move(current_evidence_refs_value)),
      current_evidence(std::move(current_evidence_value)),
      replay_evidence_refs(std::move(replay_evidence_refs_value)),
      candidates(std::move(candidates_value)), proposals(std::move(proposals_value)),
      executed_cores(std::move(executed_cores_value)), fanout_used(fanout_used_value),
      elapsed_ns(elapsed_ns_value), unresolved_conflict(unresolved_conflict_value),
      insufficient_evidence(insufficient_evidence_value),
      should_abstain(should_abstain_value),
      semantic_judgment_formed(semantic_judgment_formed_value),
      persistent_state_count(persistent_state_count_value),
      persistent_state_mutated(persistent_state_mutated_value),
      manager_retained(manager_retained_value),
      worker_state_retained(worker_state_retained_value),
      action_authorized(action_authorized_value),
      persistent_write_authorized(persistent_write_authorized_value),
      semantic_promotion_authorized(semantic_promotion_authorized_value),
      world_write_authorized(world_write_authorized_value),
      model_update_authorized(model_update_authorized_value),
      distribution_authorized(distribution_authorized_value),
      p3_authorized(p3_authorized_value) {
    if (schema_version != "rozephine-main-re-evidence-receipt-v2")
        throw std::invalid_argument("main Re-evidence receipt schema changed");
    if (verdict != "support" && verdict != "refute" && verdict != "insufficient" &&
        verdict != "conflict")
        throw std::invalid_argument("main Re-evidence verdict changed");
    if (unresolved_conflict != (verdict == "conflict"))
        throw std::invalid_argument("main conflict receipt changed");
    if (insufficient_evidence != (verdict == "insufficient"))
        throw std::invalid_argument("main insufficiency receipt changed");
    if (should_abstain != (verdict == "conflict" || verdict == "insufficient"))
        throw std::invalid_argument("unsafe main abstention boundary");
    if (semantic_judgment_formed != (verdict == "support" || verdict == "refute"))
        throw std::invalid_argument("main semantic judgment boundary changed");
    if (persistent_state_count != 1 || persistent_state_mutated)
        throw std::invalid_argument("sole-main persistent state boundary changed");
    if (manager_retained || worker_state_retained)
        throw std::invalid_argument("request-local manager or worker survived");
    if (action_authorized || persistent_write_authorized ||
        semantic_promotion_authorized || world_write_authorized ||
        model_update_authorized || distribution_authorized || p3_authorized)
        throw std::invalid_argument(
            "main Re-evidence receipt grants forbidden authority");

    std::unordered_set<std::string> unique_candidate_ids;
    std::vector<std::string> selected_ids;
    for (const auto& candidate : candidates) {
        if (!unique_candidate_ids.insert(candidate.episode_id).second)
            throw std::invalid_argument("main candidate receipt contains duplicates");
        if (candidate.selected) selected_ids.push_back(candidate.episode_id);
    }
    const std::vector<std::string> expected_selected =
        selected_episode_id ? std::vector<std::string>{*selected_episode_id}
                            : std::vector<std::string>{};
    if (selected_ids != expected_selected)
        throw std::invalid_argument(
            "main selected and rejected candidate partition changed");

    std::vector<std::string> evidence_partition;
    evidence_partition.reserve(current_evidence.size());
    for (const auto& item : current_evidence)
        evidence_partition.push_back(item.evidence_ref);
    if (evidence_partition != current_evidence_refs)
        throw std::invalid_argument("main current evidence receipt partition changed");
}

}  // namespace swegca::world
