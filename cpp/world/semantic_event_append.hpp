#pragma once

#include "world/semantic_vrs_ingress.hpp"
#include "world/session_semantic_binding.hpp"
#include "world/vrs_canonicalization.hpp"
#include "world/vrs_edge_address_index.hpp"
#include "world/vrs_event_delta.hpp"
#include "world/vrs_sparse_lineage.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view semantic_event_append_source_sha256 =
    "816a39039c3b84733cd7b8d4d0b6d6486ca77528b30e144db74bd52d88a7a619";
inline constexpr std::string_view semantic_event_append_schema =
    "rozephine-semantic-event-append-v1";

struct SemanticAppendProposalReceipt final {
    SemanticGraphReceipt semantic;
    std::vector<std::uint32_t> canonical_edge_ids;
};

struct SemanticEventAppendReceipt final {
    std::string schema;
    std::string parent_memory_snapshot_id;
    std::string candidate_snapshot_id;
    std::size_t added_terms{};
    std::size_t added_canonical_groups{};
    std::size_t added_logical_members{};
    std::size_t touched_canonical_groups{};
    std::vector<SemanticAppendProposalReceipt> semantic_proposals;
    std::vector<std::pair<std::string, std::uint64_t>> stages_ns;
    std::uint64_t total_ns{};

    [[nodiscard]] constexpr std::uint64_t original_episode_reingestions() const noexcept {
        return 0;
    }
    [[nodiscard]] constexpr std::uint64_t new_observation_count() const noexcept { return 0; }
    [[nodiscard]] constexpr std::uint64_t independent_evidence_count() const noexcept { return 0; }
    [[nodiscard]] constexpr std::uint64_t parent_numeric_materializations() const noexcept {
        return 0;
    }
    [[nodiscard]] constexpr std::uint64_t parent_term_enumerations() const noexcept { return 0; }
    [[nodiscard]] constexpr std::uint64_t parent_lineage_materializations() const noexcept {
        return 0;
    }
    [[nodiscard]] constexpr std::uint64_t internal_llm_calls() const noexcept { return 0; }
    [[nodiscard]] constexpr bool main_pair_committed() const noexcept { return false; }
    [[nodiscard]] constexpr bool authority_granted() const noexcept { return false; }
    [[nodiscard]] constexpr bool signal_settled() const noexcept { return false; }
    [[nodiscard]] constexpr bool whole_graph_convergence_claimed() const noexcept {
        return false;
    }
};

// One uncommitted semantic graph candidate. This object does not settle event
// signals, publish hot addresses, perform Main CAS, or grant authority.
class SemanticEventAppend final {
public:
    SemanticEventAppend(
        std::shared_ptr<const EventSignalInputs> parent,
        std::shared_ptr<const EventSignalInputs> inputs,
        std::shared_ptr<const TermAddressIndex> address_index,
        std::shared_ptr<const CanonicalEdgeAddressIndex> edge_address_index,
        SparseCanonicalLineage lineage,
        SemanticGraphDelta semantic_delta,
        CanonicalVrsAppendDelta canonical_delta,
        std::vector<std::pair<std::string, std::uint64_t>> timings_ns);

    const std::shared_ptr<const EventSignalInputs> parent;
    const std::shared_ptr<const EventSignalInputs> inputs;
    const std::shared_ptr<const TermAddressIndex> address_index;
    const std::shared_ptr<const CanonicalEdgeAddressIndex> edge_address_index;
    const SparseCanonicalLineage lineage;
    const SemanticGraphDelta semantic_delta;
    const CanonicalVrsAppendDelta canonical_delta;
    const std::vector<std::pair<std::string, std::uint64_t>> timings_ns;

    [[nodiscard]] SemanticEventAppendReceipt receipt() const;
};

struct SessionSemanticAppendProposalReceipt final {
    SessionSemanticGraphReceipt semantic;
    std::vector<std::uint32_t> canonical_edge_ids;
};

struct SessionSemanticEventAppendReceipt final {
    std::string schema;
    std::string parent_memory_snapshot_id;
    std::string candidate_snapshot_id;
    std::size_t added_terms{};
    std::size_t added_canonical_groups{};
    std::size_t added_logical_members{};
    std::size_t touched_canonical_groups{};
    std::vector<SessionSemanticAppendProposalReceipt> semantic_proposals;
    std::vector<std::pair<std::string, std::uint64_t>> stages_ns;
    std::uint64_t total_ns{};

    [[nodiscard]] constexpr std::uint64_t original_episode_reingestions() const noexcept { return 0; }
    [[nodiscard]] constexpr std::uint64_t new_observation_count() const noexcept { return 0; }
    [[nodiscard]] constexpr std::uint64_t independent_evidence_count() const noexcept { return 0; }
    [[nodiscard]] constexpr std::uint64_t parent_numeric_materializations() const noexcept { return 0; }
    [[nodiscard]] constexpr std::uint64_t parent_term_enumerations() const noexcept { return 0; }
    [[nodiscard]] constexpr std::uint64_t parent_lineage_materializations() const noexcept { return 0; }
    [[nodiscard]] constexpr std::uint64_t internal_llm_calls() const noexcept { return 0; }
    [[nodiscard]] constexpr bool main_pair_committed() const noexcept { return false; }
    [[nodiscard]] constexpr bool authority_granted() const noexcept { return false; }
    [[nodiscard]] constexpr bool signal_settled() const noexcept { return false; }
    [[nodiscard]] constexpr bool whole_graph_convergence_claimed() const noexcept { return false; }
};

// The optional Python session_binding branch has a distinct typed receipt in
// C++, while retaining the same outer append schema and sparse candidate rules.
class SessionSemanticEventAppend final {
public:
    SessionSemanticEventAppend(
        std::shared_ptr<const EventSignalInputs> parent,
        std::shared_ptr<const EventSignalInputs> inputs,
        std::shared_ptr<const TermAddressIndex> address_index,
        std::shared_ptr<const CanonicalEdgeAddressIndex> edge_address_index,
        SparseCanonicalLineage lineage,
        SessionSemanticGraphDelta semantic_delta,
        CanonicalVrsAppendDelta canonical_delta,
        std::vector<std::pair<std::string, std::uint64_t>> timings_ns);

    const std::shared_ptr<const EventSignalInputs> parent;
    const std::shared_ptr<const EventSignalInputs> inputs;
    const std::shared_ptr<const TermAddressIndex> address_index;
    const std::shared_ptr<const CanonicalEdgeAddressIndex> edge_address_index;
    const SparseCanonicalLineage lineage;
    const SessionSemanticGraphDelta semantic_delta;
    const CanonicalVrsAppendDelta canonical_delta;
    const std::vector<std::pair<std::string, std::uint64_t>> timings_ns;

    [[nodiscard]] SessionSemanticEventAppendReceipt receipt() const;
};

[[nodiscard]] SemanticEventAppend prepare_semantic_event_append(
    std::shared_ptr<const EventSignalInputs> parent,
    std::shared_ptr<const TermAddressIndex> address_index,
    std::shared_ptr<const CanonicalVrsMemberLineage> lineage,
    std::shared_ptr<const CanonicalEdgeAddressIndex> edge_address_index,
    const std::vector<SemanticSourceEpisode>& episodes,
    const std::vector<SemanticEncoding>& proposals,
    std::string snapshot_id);

[[nodiscard]] SessionSemanticEventAppend prepare_semantic_event_append(
    std::shared_ptr<const EventSignalInputs> parent,
    std::shared_ptr<const TermAddressIndex> address_index,
    std::shared_ptr<const CanonicalVrsMemberLineage> lineage,
    std::shared_ptr<const CanonicalEdgeAddressIndex> edge_address_index,
    std::shared_ptr<const BoundSessionSemantics> session_binding,
    std::string snapshot_id,
    std::string parent_vrs_snapshot_id);

[[nodiscard]] SessionSemanticEventAppend prepare_semantic_event_append(
    std::shared_ptr<const EventSignalInputs> parent,
    std::shared_ptr<const TermAddressIndex> address_index,
    const SparseCanonicalLineage& lineage,
    std::shared_ptr<const CanonicalEdgeAddressIndex> edge_address_index,
    std::shared_ptr<const BoundSessionSemantics> session_binding,
    std::string snapshot_id,
    std::string parent_vrs_snapshot_id);

[[nodiscard]] SemanticEventAppend prepare_semantic_event_append(
    std::shared_ptr<const EventSignalInputs> parent,
    std::shared_ptr<const TermAddressIndex> address_index,
    const SparseCanonicalLineage& lineage,
    std::shared_ptr<const CanonicalEdgeAddressIndex> edge_address_index,
    const std::vector<SemanticSourceEpisode>& episodes,
    const std::vector<SemanticEncoding>& proposals,
    std::string snapshot_id);

}  // namespace swegca::world
