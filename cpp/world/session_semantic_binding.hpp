#pragma once

#include "world/semantic_vrs_ingress.hpp"
#include "world/prepared_session_cache.hpp"
#include "world/session_speech_ingress.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view session_semantic_binding_source_sha256 =
    "5886bec1406335b3f6bc4e56bf247cd070687b4a9c6fa107d4d30cf24d089060";
inline constexpr std::string_view session_semantic_input_schema =
    "rozephine-session-semantic-input-v1";
inline constexpr std::string_view session_semantic_graph_schema =
    "rozephine-session-semantic-graph-v1";

// The source module binds exact prepared events, occurrences and context before
// graph preparation. They remain attached here as opaque source views; graph
// preparation consumes only the already rebound anchors and meaning unit.
struct SessionSemanticUnitBinding final {
    SemanticMeaningUnit unit;
    std::vector<SemanticAnchor> anchors;
    JsonValue::Array events;
    JsonValue::Array occurrences;
    JsonValue::Array input_context;
    std::vector<std::string> unresolved_anchors;

    [[nodiscard]] constexpr bool source_interpretation_verified() const noexcept {
        return false;
    }
    [[nodiscard]] constexpr bool grants_authority() const noexcept { return false; }
};

// A COLD, already source-bound view. This is not an original MemoryEpisode.
// Every reconstruction parent remains explicit in original_episodes.
struct BoundSessionSemantics final {
    std::string memory_snapshot_id;
    std::vector<std::string> document_key;
    JsonValue interpretation_receipt;
    std::vector<SemanticSourceEpisode> original_episodes;
    SemanticSourceEpisode derivative;
    std::vector<SemanticAnchor> source_parts;
    std::vector<SessionSemanticUnitBinding> units;
    std::vector<std::string> unresolved;

    [[nodiscard]] constexpr std::uint64_t new_observation_count() const noexcept {
        return 0;
    }
    [[nodiscard]] constexpr std::uint64_t independent_evidence_count() const noexcept {
        return 0;
    }
    [[nodiscard]] constexpr bool grants_authority() const noexcept { return false; }
};

struct SessionSemanticEdgeRole final {
    std::string kind;
    std::string source_address;
    std::string target_address;
    friend bool operator==(const SessionSemanticEdgeRole&,
                           const SessionSemanticEdgeRole&) = default;
};

struct SessionSemanticGraphReceipt final {
    std::string schema;
    JsonValue interpretation;
    std::string episode_id;
    std::size_t member_edge_start{};
    std::size_t member_edge_count{};
    std::vector<SessionSemanticEdgeRole> edge_roles;

    [[nodiscard]] constexpr bool associations_are_logical_implications() const noexcept {
        return false;
    }
    [[nodiscard]] constexpr std::uint64_t independent_evidence_count() const noexcept {
        return 0;
    }
    [[nodiscard]] constexpr std::uint64_t new_observation_count() const noexcept {
        return 0;
    }
    [[nodiscard]] constexpr bool grants_authority() const noexcept { return false; }
};

class SessionSemanticGraphDelta final {
public:
    SessionSemanticGraphDelta(
        std::shared_ptr<const TermAddressIndex> address_index,
        std::string memory_snapshot_id,
        std::string vrs_snapshot_id,
        std::size_t edge_start,
        std::shared_ptr<const BoundSessionSemantics> binding,
        std::vector<std::string> appended_terms,
        std::vector<EventSignalEdge> edge_rows,
        std::vector<SessionSemanticEdgeRole> edge_roles,
        std::vector<std::vector<SessionSemanticEdgeRole>> unit_graph_addresses);

    const std::shared_ptr<const TermAddressIndex> address_index;
    const std::string memory_snapshot_id;
    const std::string vrs_snapshot_id;
    const std::size_t edge_start;
    const std::shared_ptr<const BoundSessionSemantics> binding;
    const std::vector<std::string> appended_terms;
    const std::vector<EventSignalEdge> edge_rows;
    const std::vector<SessionSemanticEdgeRole> edge_roles;
    const std::vector<std::vector<SessionSemanticEdgeRole>> unit_graph_addresses;

    [[nodiscard]] constexpr bool associations_are_logical_implications() const noexcept {
        return false;
    }
    [[nodiscard]] constexpr bool grants_authority() const noexcept { return false; }
    [[nodiscard]] std::vector<SessionSemanticGraphReceipt> receipts() const;
    void require_parent(
        const std::shared_ptr<const TermAddressIndex>& address_index,
        std::string_view memory_snapshot_id,
        std::string_view vrs_snapshot_id,
        std::size_t edge_count) const;
};

[[nodiscard]] SessionSemanticGraphDelta prepare_session_semantic_delta(
    std::shared_ptr<const BoundSessionSemantics> bound,
    std::shared_ptr<const TermAddressIndex> address_index,
    std::size_t edge_count,
    std::string memory_snapshot_id,
    std::string vrs_snapshot_id);

[[nodiscard]] std::shared_ptr<const BoundSessionSemantics> bind_session_semantics(
    const PreparedSessionView& view,
    const std::vector<SemanticSourceEpisode>& source_episodes,
    PreparedSessionEntryPtr entry,
    const SessionSpeechInterpretation& interpretation);

}  // namespace swegca::world
