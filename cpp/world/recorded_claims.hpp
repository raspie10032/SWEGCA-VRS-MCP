#pragma once

#include "world/semantic_comparison.hpp"
#include "world/semantic_encoding.hpp"
#include "world/session_content_encoding.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view recorded_claims_source_sha256 =
    "47d1dfdb555dafe16ba69ce601daf5539949b3fefa8d154648364b99a232304c";

struct RecordedClaimAddress final {
    std::string episode_id;
    std::size_t step{};
    std::size_t claim{};
    friend bool operator==(const RecordedClaimAddress&,
                           const RecordedClaimAddress&) = default;
};

using RecordedSourceContext =
    std::variant<SemanticSourceContext, SessionSourceContext>;

struct RecordedSemanticAnchorResolution final {
    std::vector<std::string> referenced_anchors;
    std::vector<std::string> unresolved_anchors;
    std::size_t other_unresolved_anchor_count{};
    std::vector<std::size_t> attributable_input_steps;
    bool response_incomplete{};
    std::vector<RecordedSourceContext> input_context;
    std::vector<std::string> attributable_document_key;
};

using RecordedGraphAddresses = std::variant<
    std::monostate,
    std::vector<SemanticGraphCoordinate>,
    std::vector<SessionSemanticEdgeRole>>;

struct RecordedEncodedClaim final {
    RecordedClaimAddress address;
    JsonValue subject;
    std::string predicate;
    JsonValue value;
    std::optional<double> reported_confidence;
    std::optional<SemanticMeaningUnit> semantic_unit;
    std::optional<SessionSemanticPropositionKey> semantic_key;
    RecordedGraphAddresses semantic_graph_addresses;
    std::vector<std::string> unresolved;
    std::optional<RecordedSemanticAnchorResolution> semantic_anchor_resolution;
};

using RecordedSemanticEncoding = std::variant<
    std::monostate,
    SemanticEncoding,
    std::shared_ptr<const SessionContentEncoding>>;

struct RecordedEncodedEvent final {
    std::string episode_id;
    std::size_t step{};
    JsonValue event_id;
    JsonValue event_type;
    JsonValue evidence_kind;
    std::vector<RecordedEncodedClaim> claims;
    JsonValue declared_source;
    std::vector<std::string> declared_evidence_refs;
    std::vector<std::string> source_addresses;
    std::vector<std::string> evidence_refs;
    std::string revision;
    std::string outcome;
    std::string verification_state;
    std::vector<std::string> unresolved;
    RecordedSemanticEncoding semantic_encoding;
    std::vector<std::size_t> interpreted_input_steps;

    [[nodiscard]] constexpr bool grants_authority() const noexcept { return false; }
};

using RecordedSourceLookup =
    std::function<const SemanticSourceEpisode*(std::string_view)>;

[[nodiscard]] bool is_recorded_event(const JsonValue& observation) noexcept;

// Cold preparation only. A null optional means the observation is not a
// recorded event. Malformed recorded and semantic events remain represented by
// an EncodedEvent carrying unresolved reasons; they are never hidden as absent.
[[nodiscard]] std::optional<RecordedEncodedEvent> prepare_recorded_event(
    const SemanticSourceEpisode& episode,
    std::size_t ordinal,
    const RecordedSourceLookup& source_lookup = {},
    const PreparedSessionView* source_memory = nullptr,
    const std::vector<SemanticSourceEpisode>& source_episodes = {});

}  // namespace swegca::world
