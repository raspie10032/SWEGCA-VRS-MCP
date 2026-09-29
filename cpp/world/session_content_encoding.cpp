#include "world/session_content_encoding.hpp"

#include <map>
#include <stdexcept>
#include <utility>

namespace swegca::world {

SessionContentEncoding::SessionContentEncoding(
    std::shared_ptr<const BoundSessionSemantics> binding_value)
    : binding(std::move(binding_value)) {
    if (!binding || binding->document_key.size() < 2)
        throw std::invalid_argument("source-bound session encoding required");
}

std::string_view SessionContentEncoding::source_id() const {
    return binding->document_key[0];
}

std::string_view SessionContentEncoding::source_revision() const {
    return binding->document_key[1];
}

std::string_view SessionContentEncoding::source_digest() const {
    return source_revision();
}

std::vector<std::string> SessionContentEncoding::source_episode_ids() const {
    std::vector<std::string> result;
    result.reserve(binding->original_episodes.size());
    for (const auto& episode : binding->original_episodes)
        result.push_back(episode.episode_id);
    return result;
}

const std::vector<std::string>& SessionContentEncoding::source_addresses() const {
    return binding->derivative.source_addresses;
}

const std::vector<SemanticAnchor>& SessionContentEncoding::anchors() const {
    return binding->source_parts;
}

std::vector<SemanticMeaningUnit> SessionContentEncoding::units() const {
    std::vector<SemanticMeaningUnit> result;
    result.reserve(binding->units.size());
    for (const auto& row : binding->units) result.push_back(row.unit);
    return result;
}

const std::vector<std::string>& SessionContentEncoding::unresolved() const {
    return binding->unresolved;
}

const std::vector<std::string>& SessionContentEncoding::document_key() const {
    return binding->document_key;
}

std::vector<std::vector<SessionSemanticEdgeRole>>
claim_graph_addresses(const BoundSessionSemantics& bound) {
    const auto scope = [&] {
        JsonValue::Array values;
        for (const auto& value : bound.document_key) values.emplace_back(value);
        return JsonValue(std::move(values));
    }();
    const auto document = semantic_scoped_address("session-document", scope, scope);
    std::map<std::string, std::string, std::less<>> anchors;
    for (const auto& anchor : bound.source_parts)
        anchors.emplace(anchor.identifier, semantic_anchor_address(scope, anchor));
    std::vector<std::vector<SessionSemanticEdgeRole>> result;
    result.reserve(bound.units.size());
    for (const auto& row : bound.units) {
        const auto address = semantic_unit_address(scope, row.unit);
        std::vector<SessionSemanticEdgeRole> links{
            {"source_derivation", document, bound.derivative.episode_id},
            {"encoding_unit", bound.derivative.episode_id, address}};
        for (const auto& anchor : row.anchors) {
            const auto found = anchors.find(anchor.identifier);
            if (found == anchors.end())
                throw std::invalid_argument("session content anchor unavailable");
            links.push_back({"unit_anchor_proposal", address, found->second});
            links.push_back({"anchor_document_location", found->second, document});
        }
        result.push_back(std::move(links));
    }
    return result;
}

}  // namespace swegca::world
