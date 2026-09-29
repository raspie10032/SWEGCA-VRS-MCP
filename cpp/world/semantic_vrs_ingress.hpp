#pragma once

#include "world/cognitive_state.hpp"
#include "world/term_address_index.hpp"
#include "world/vrs_event_signal.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view semantic_vrs_ingress_source_sha256 =
    "213118a20f197018f3071b1b4fb6cbcfcbdb21265566b338327cddc23cf53f5e";
inline constexpr std::string_view semantic_vrs_projection_schema =
    "rozephine-semantic-vrs-projection-v1";
inline constexpr std::string_view semantic_encoding_schema =
    "rozephine-semantic-encoding-v1";

struct SemanticMemoryStep final {
    std::string phase;
    JsonValue observation;
    std::vector<std::string> relations;
    std::string judgment;
    std::string outcome;
    std::vector<std::string> evidence_refs;
};

// Detached source fields required by the Python source_payload/source_digest
// contract. This type carries no memory lookup or write authority.
struct SemanticSourceEpisode final {
    std::string episode_id;
    std::vector<std::string> cues;
    std::vector<SemanticMemoryStep> steps;
    std::vector<std::string> source_addresses;
    std::string revision;
    std::string verification_state;
};

using SemanticPathElement = std::variant<std::int64_t, std::string>;

struct SemanticAnchor final {
    std::string identifier;
    std::int64_t step{};
    std::vector<SemanticPathElement> path;
    std::string modality;
    std::string role{"original"};
    std::vector<std::int64_t> char_range;
    std::vector<std::int64_t> region;
    std::vector<std::int64_t> time_ns;
};

struct SemanticQualifier final {
    std::string kind;
    std::string value;
    std::vector<std::string> anchors;
};

struct SemanticMeaningUnit final {
    std::string subject;
    std::string predicate;
    JsonValue value;
    std::string polarity;
    std::string basis;
    std::vector<std::string> anchors;
    std::vector<SemanticQualifier> qualifiers;
    std::string value_kind{"unspecified"};
};

// A source-bound, already produced semantic proposal. Ingress revalidates its
// complete source digest, revision, addresses, outcomes, anchor coordinates,
// and structural anchor coverage before it creates graph coordinates.
struct SemanticEncoding final {
    std::string source_id;
    std::string source_revision;
    std::string source_digest;
    std::vector<std::string> source_addresses;
    std::vector<std::string> outcomes;
    std::string model;
    std::vector<SemanticAnchor> anchors;
    std::vector<SemanticMeaningUnit> units;
    std::vector<std::string> unresolved;
    std::vector<std::size_t> partial_response_units;
    JsonValue::Array input_context;
};

struct SemanticGraphCoordinate final {
    std::string kind;
    std::string source;
    std::string target;
    friend bool operator==(const SemanticGraphCoordinate&,
                           const SemanticGraphCoordinate&) = default;
};

struct SemanticEdgeRole final {
    std::string kind;
    std::uint32_t source_node{};
    std::uint32_t target_node{};
    std::optional<std::string> anchor_id;
    std::optional<std::size_t> unit_index;
    friend bool operator==(const SemanticEdgeRole&, const SemanticEdgeRole&) = default;
};

struct SemanticGraphReceipt final {
    SemanticEncoding encoding;
    std::string episode_id;
    std::uint32_t node_id{};
    std::vector<std::pair<std::string, std::uint32_t>> anchor_nodes;
    std::vector<std::uint32_t> unit_nodes;
    std::size_t member_edge_start{};
    std::vector<SemanticEdgeRole> edge_roles;

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

// Source-bound graph additions only. The parent address directory is retained
// by identity; no parent terms, numeric arrays, or edge rows are materialized.
class SemanticGraphDelta final {
public:
    SemanticGraphDelta(
        std::shared_ptr<const TermAddressIndex> address_index,
        std::size_t edge_start,
        std::vector<std::string> appended_terms,
        std::vector<EventSignalEdge> edge_rows,
        std::vector<SemanticGraphReceipt> records);

    const std::shared_ptr<const TermAddressIndex> address_index;
    const std::size_t edge_start;
    const std::vector<std::string> appended_terms;
    const std::vector<EventSignalEdge> edge_rows;

    [[nodiscard]] std::vector<SemanticGraphReceipt> receipts() const { return records_; }

private:
    const std::vector<SemanticGraphReceipt> records_;
};

[[nodiscard]] std::string semantic_source_digest(const SemanticSourceEpisode& source);
[[nodiscard]] std::string semantic_canonical_json(const JsonValue& value);
[[nodiscard]] std::string semantic_json_digest(const JsonValue& value);
[[nodiscard]] std::string semantic_encoding_episode_id(const SemanticEncoding& encoding);
[[nodiscard]] JsonValue semantic_encoding_receipt(const SemanticEncoding& encoding);
[[nodiscard]] JsonValue semantic_meaning_unit_payload(
    const SemanticMeaningUnit& unit, bool address_form = false);
void validate_semantic_encoding(const SemanticEncoding& encoding,
                                const SemanticSourceEpisode& source);
[[nodiscard]] std::string semantic_scoped_address(
    std::string_view kind, const JsonValue& scope, const JsonValue& value);
[[nodiscard]] std::string semantic_anchor_address(
    std::string_view source_id, std::string_view source_revision,
    const SemanticAnchor& anchor);
[[nodiscard]] std::string semantic_anchor_address(
    const JsonValue& scope, const SemanticAnchor& anchor);
[[nodiscard]] std::string semantic_unit_address(
    std::string_view source_id, std::string_view source_revision,
    const SemanticMeaningUnit& unit);
[[nodiscard]] std::string semantic_unit_address(
    const JsonValue& scope, const SemanticMeaningUnit& unit);

[[nodiscard]] std::vector<std::vector<SemanticGraphCoordinate>>
prepare_claim_graph_addresses(const SemanticEncoding& encoding,
                              std::string_view derivative_id);

[[nodiscard]] SemanticGraphDelta prepare_semantic_graph_delta(
    std::shared_ptr<const TermAddressIndex> address_index,
    std::size_t edge_count,
    const std::vector<SemanticSourceEpisode>& episodes,
    const std::vector<SemanticEncoding>& proposals);

}  // namespace swegca::world
