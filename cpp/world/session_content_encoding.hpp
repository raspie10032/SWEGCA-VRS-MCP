#pragma once

#include "world/session_semantic_binding.hpp"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view session_content_encoding_source_sha256 =
    "e62419a5fec3ea7b5356e26949501d08459443ba3932183212da389e23b95d2b";

class SessionContentEncoding final {
public:
    explicit SessionContentEncoding(std::shared_ptr<const BoundSessionSemantics> binding);

    const std::shared_ptr<const BoundSessionSemantics> binding;

    [[nodiscard]] constexpr std::string_view source_kind() const noexcept {
        return "session_document";
    }
    [[nodiscard]] std::string_view source_id() const;
    [[nodiscard]] std::string_view source_revision() const;
    [[nodiscard]] std::string_view source_digest() const;
    [[nodiscard]] std::vector<std::string> source_episode_ids() const;
    [[nodiscard]] const std::vector<std::string>& source_addresses() const;
    [[nodiscard]] std::vector<std::string> outcomes() const;
    [[nodiscard]] std::string_view model() const;
    [[nodiscard]] const std::vector<SemanticAnchor>& anchors() const;
    [[nodiscard]] std::vector<SemanticMeaningUnit> units() const;
    [[nodiscard]] const std::vector<std::string>& unresolved() const;
    [[nodiscard]] const std::vector<std::string>& document_key() const;
    [[nodiscard]] const JsonValue& input_context() const;
    [[nodiscard]] const JsonValue& parent_fragments() const;
    [[nodiscard]] constexpr std::vector<std::size_t> partial_response_units() const {
        return {};
    }
};

[[nodiscard]] std::vector<std::vector<SessionSemanticEdgeRole>>
claim_graph_addresses(const BoundSessionSemantics& bound);

struct SessionRecordedClaimAddress final {
    std::string episode_id;
    std::size_t step{};
    std::size_t claim{};
    friend bool operator==(const SessionRecordedClaimAddress&,
                           const SessionRecordedClaimAddress&) = default;
};

struct SessionSemanticPropositionKey final {
    std::string subject;
    std::string predicate;
    std::string value_type;
    JsonValue value;
    std::vector<std::pair<std::string, std::string>> qualifiers;
    std::string value_kind;
    friend bool operator==(const SessionSemanticPropositionKey&,
                           const SessionSemanticPropositionKey&) = default;
};

struct SessionSemanticAnchorResolution final {
    std::vector<std::string> referenced_anchors;
    std::vector<std::string> unresolved_anchors;
    std::size_t other_unresolved_anchor_count{};
    JsonValue::Array input_context;
    std::vector<std::string> attributable_document_key;
    [[nodiscard]] constexpr bool response_incomplete() const noexcept { return false; }
};

struct SessionEncodedClaim final {
    SessionRecordedClaimAddress address;
    std::string subject;
    std::string predicate;
    JsonValue value;
    SemanticMeaningUnit semantic_unit;
    SessionSemanticPropositionKey semantic_key;
    std::vector<SessionSemanticEdgeRole> semantic_graph_addresses;
    SessionSemanticAnchorResolution semantic_anchor_resolution;
};

struct SessionEncodedEvent final {
    std::string episode_id;
    std::size_t step{};
    std::string event_id;
    std::string event_type;
    std::string evidence_kind;
    std::vector<SessionEncodedClaim> claims;
    std::vector<std::string> source_addresses;
    std::vector<std::string> evidence_refs;
    std::string revision;
    std::string outcome;
    std::string verification_state;
    std::vector<std::string> unresolved;
    std::shared_ptr<const SessionContentEncoding> semantic_encoding;

    [[nodiscard]] constexpr bool grants_authority() const noexcept { return false; }
    [[nodiscard]] constexpr std::vector<std::size_t> interpreted_input_steps() const {
        return {};
    }
};

[[nodiscard]] SessionEncodedEvent prepare_session_recorded_event(
    const SemanticSourceEpisode& episode,
    std::size_t ordinal,
    const PreparedSessionView& source_memory,
    const std::vector<SemanticSourceEpisode>& source_episodes);

}  // namespace swegca::world
