#include "world/session_semantic_binding.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace swegca::world {
namespace {

[[noreturn]] void reject(const char* reason) { throw std::invalid_argument(reason); }

[[nodiscard]] bool text(const std::string_view value) noexcept {
    return value.find_first_not_of(" \t\r\n\f\v") != std::string_view::npos;
}

[[nodiscard]] JsonValue strings(const std::vector<std::string>& values) {
    JsonValue::Array result;
    result.reserve(values.size());
    for (const auto& value : values) result.emplace_back(value);
    return JsonValue(std::move(result));
}

[[nodiscard]] bool same_anchor(const SemanticAnchor& left,
                               const SemanticAnchor& right) {
    return left.identifier == right.identifier && left.step == right.step &&
        left.path == right.path && left.modality == right.modality &&
        left.role == right.role && left.char_range == right.char_range &&
        left.region == right.region && left.time_ns == right.time_ns;
}

void validate_binding(const BoundSessionSemantics& bound) {
    if (!text(bound.memory_snapshot_id) || bound.document_key.empty() ||
        !std::ranges::all_of(bound.document_key, [](const auto& value) { return text(value); }) ||
        !text(bound.derivative.episode_id))
        reject("session_semantic_graph_parent_required");

    std::unordered_set<std::string> parents;
    for (const auto& parent : bound.original_episodes)
        if (!text(parent.episode_id) || !parents.insert(parent.episode_id).second)
            reject("session_semantic_graph_parent_required");

    std::unordered_map<std::string, const SemanticAnchor*> anchors;
    for (const auto& anchor : bound.source_parts)
        if (!text(anchor.identifier) || !anchors.emplace(anchor.identifier, &anchor).second)
            reject("session_semantic_graph_parent_required");

    for (const auto& row : bound.units) {
        std::vector<std::string> references = row.unit.anchors;
        for (const auto& qualifier : row.unit.qualifiers)
            references.insert(references.end(), qualifier.anchors.begin(), qualifier.anchors.end());
        std::vector<std::string> expected;
        std::unordered_set<std::string> seen;
        for (const auto& identifier : references)
            if (seen.insert(identifier).second) expected.push_back(identifier);
        if (expected.size() != row.anchors.size())
            reject("session_semantic_graph_parent_required");
        for (std::size_t index = 0; index < expected.size(); ++index)
            if (!anchors.contains(expected[index]) ||
                row.anchors[index].identifier != expected[index] ||
                !same_anchor(row.anchors[index], *anchors.at(expected[index])))
                reject("session_semantic_graph_parent_required");
    }
    for (const auto& identifier : bound.unresolved)
        if (!anchors.contains(identifier)) reject("session_semantic_graph_parent_required");
}

}  // namespace

SessionSemanticGraphDelta::SessionSemanticGraphDelta(
    std::shared_ptr<const TermAddressIndex> address_index_value,
    std::string memory_snapshot_id_value,
    std::string vrs_snapshot_id_value,
    const std::size_t edge_start_value,
    std::shared_ptr<const BoundSessionSemantics> binding_value,
    std::vector<std::string> appended_terms_value,
    std::vector<EventSignalEdge> edge_rows_value,
    std::vector<SessionSemanticEdgeRole> edge_roles_value,
    std::vector<std::vector<SessionSemanticEdgeRole>> unit_graph_addresses_value)
    : address_index(std::move(address_index_value)),
      memory_snapshot_id(std::move(memory_snapshot_id_value)),
      vrs_snapshot_id(std::move(vrs_snapshot_id_value)),
      edge_start(edge_start_value), binding(std::move(binding_value)),
      appended_terms(std::move(appended_terms_value)),
      edge_rows(std::move(edge_rows_value)), edge_roles(std::move(edge_roles_value)),
      unit_graph_addresses(std::move(unit_graph_addresses_value)) {
    if (!address_index || !binding || edge_rows.size() != edge_roles.size() ||
        unit_graph_addresses.size() != binding->units.size())
        reject("session_semantic_graph_parent_required");
}

std::vector<SessionSemanticGraphReceipt> SessionSemanticGraphDelta::receipts() const {
    return {{std::string(session_semantic_graph_schema), binding->interpretation_receipt,
             binding->derivative.episode_id, edge_start, edge_rows.size(), edge_roles}};
}

void SessionSemanticGraphDelta::require_parent(
    const std::shared_ptr<const TermAddressIndex>& expected_address_index,
    const std::string_view expected_memory_snapshot_id,
    const std::string_view expected_vrs_snapshot_id,
    const std::size_t edge_count) const {
    if (expected_address_index.get() != address_index.get() ||
        expected_memory_snapshot_id != memory_snapshot_id ||
        expected_vrs_snapshot_id != vrs_snapshot_id || edge_count != edge_start)
        reject("session_semantic_graph_parent_changed");
}

SessionSemanticGraphDelta prepare_session_semantic_delta(
    std::shared_ptr<const BoundSessionSemantics> bound,
    std::shared_ptr<const TermAddressIndex> address_index,
    const std::size_t edge_count,
    std::string memory_snapshot_id,
    std::string vrs_snapshot_id) {
    if (!bound || !address_index || bound->memory_snapshot_id != memory_snapshot_id ||
        !text(memory_snapshot_id) || !text(vrs_snapshot_id))
        reject("session_semantic_graph_parent_required");
    validate_binding(*bound);

    for (const auto& parent : bound->original_episodes)
        if (!address_index->lookup(parent.episode_id))
            reject("session_semantic_graph_parent_missing");
    if (address_index->lookup(bound->derivative.episode_id))
        reject("session_semantic_derivative_already_in_graph");

    if (address_index->size() > std::numeric_limits<std::uint32_t>::max())
        reject("session semantic node capacity exceeded");
    std::unordered_map<std::string, std::uint32_t> additions;
    std::vector<std::string> appended_terms;
    std::vector<EventSignalEdge> edge_rows;
    std::vector<SessionSemanticEdgeRole> edge_roles;
    std::set<std::tuple<std::string, std::string, std::string>> seen;

    const auto node = [&](const std::string& value) -> std::uint32_t {
        if (const auto existing = address_index->lookup(value)) {
            if (*existing > std::numeric_limits<std::uint32_t>::max())
                reject("session semantic node capacity exceeded");
            return static_cast<std::uint32_t>(*existing);
        }
        if (const auto existing = additions.find(value); existing != additions.end())
            return existing->second;
        if (appended_terms.size() > std::numeric_limits<std::uint32_t>::max() -
                address_index->size())
            reject("session semantic node capacity exceeded");
        const auto result = static_cast<std::uint32_t>(
            address_index->size() + appended_terms.size());
        additions.emplace(value, result);
        appended_terms.push_back(value);
        return result;
    };

    const auto link = [&](const std::string& left, const std::string& right,
                          const std::string& kind) {
        SessionSemanticEdgeRole role{kind, left, right};
        if (seen.emplace(kind, left, right).second) {
            edge_rows.push_back({node(left), node(right), 1, .75F});
            edge_roles.push_back(role);
        }
        return role;
    };

    const auto scope = strings(bound->document_key);
    const auto document = semantic_scoped_address("session-document", scope, scope);
    for (const auto& parent : bound->original_episodes)
        link(parent.episode_id, document, "document_reconstruction_parent");
    const auto derivation = link(document, bound->derivative.episode_id, "source_derivation");

    std::unordered_map<std::string, std::string> anchors;
    anchors.reserve(bound->source_parts.size());
    for (const auto& anchor : bound->source_parts) {
        const auto address = semantic_anchor_address(scope, anchor);
        anchors.emplace(anchor.identifier, address);
        link(address, document, "anchor_document_location");
    }

    std::vector<std::vector<SessionSemanticEdgeRole>> unit_graph_addresses;
    unit_graph_addresses.reserve(bound->units.size());
    for (const auto& row : bound->units) {
        const auto address = semantic_unit_address(scope, row.unit);
        std::vector<SessionSemanticEdgeRole> links{derivation};
        links.push_back(link(bound->derivative.episode_id, address, "encoding_unit"));
        for (const auto& anchor : row.anchors) {
            const auto& anchor_address = anchors.at(anchor.identifier);
            links.push_back(link(address, anchor_address, "unit_anchor_proposal"));
            links.push_back({"anchor_document_location", anchor_address, document});
        }
        unit_graph_addresses.push_back(std::move(links));
    }
    for (const auto& identifier : bound->unresolved)
        link(bound->derivative.episode_id, anchors.at(identifier), "unresolved_anchor");

    return SessionSemanticGraphDelta(
        std::move(address_index), std::move(memory_snapshot_id),
        std::move(vrs_snapshot_id), edge_count, std::move(bound),
        std::move(appended_terms), std::move(edge_rows), std::move(edge_roles),
        std::move(unit_graph_addresses));
}

}  // namespace swegca::world
