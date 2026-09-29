#include "world/session_content_encoding.hpp"

#include <map>
#include <set>
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

std::vector<std::string> SessionContentEncoding::outcomes() const {
    std::vector<std::string> result;
    if (!binding->entry) return result;
    result.reserve(binding->entry->document.fragments.size());
    for (const auto& fragment : binding->entry->document.fragments)
        result.push_back(fragment.outcome);
    return result;
}

std::string_view SessionContentEncoding::model() const {
    return binding->interpretation_receipt.at("model").as_string();
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

const JsonValue& SessionContentEncoding::input_context() const {
    return binding->interpretation_receipt.at("source").at("input_context");
}

const JsonValue& SessionContentEncoding::parent_fragments() const {
    return binding->interpretation_receipt.at("source").at("parent_fragments");
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

namespace {

[[nodiscard]] std::string value_type(const JsonValue& value) {
    return std::visit([](const auto& current) -> std::string {
        using T = std::decay_t<decltype(current)>;
        if constexpr (std::is_same_v<T, std::nullptr_t>) return "NoneType";
        if constexpr (std::is_same_v<T, bool>) return "bool";
        if constexpr (std::is_same_v<T, std::int64_t>) return "int";
        if constexpr (std::is_same_v<T, double>) return "float";
        if constexpr (std::is_same_v<T, std::string>) return "str";
        if constexpr (std::is_same_v<T, JsonValue::Array>) return "list";
        return "dict";
    }, value.storage());
}

[[nodiscard]] SessionSemanticPropositionKey proposition_key(
    const SemanticMeaningUnit& unit) {
    std::set<std::pair<std::string, std::string>> unique;
    for (const auto& qualifier : unit.qualifiers)
        unique.emplace(qualifier.kind, qualifier.value);
    return {unit.subject, unit.predicate, value_type(unit.value), unit.value,
            {unique.begin(), unique.end()}, unit.value_kind};
}

[[nodiscard]] bool same_episode(const SemanticSourceEpisode& left,
                                const SemanticSourceEpisode& right) {
    if (left.episode_id != right.episode_id || left.cues != right.cues ||
        left.source_addresses != right.source_addresses ||
        left.revision != right.revision ||
        left.verification_state != right.verification_state ||
        left.steps.size() != right.steps.size()) return false;
    for (std::size_t index = 0; index != left.steps.size(); ++index) {
        const auto& a = left.steps[index];
        const auto& b = right.steps[index];
        if (a.phase != b.phase || a.observation != b.observation ||
            a.relations != b.relations || a.judgment != b.judgment ||
            a.outcome != b.outcome || a.evidence_refs != b.evidence_refs)
            return false;
    }
    return true;
}

}  // namespace

SessionEncodedEvent prepare_session_recorded_event(
    const SemanticSourceEpisode& episode,
    const std::size_t ordinal,
    const PreparedSessionView& source_memory,
    const std::vector<SemanticSourceEpisode>& source_episodes) {
    if (ordinal >= episode.steps.size())
        throw std::invalid_argument("session semantic event ordinal required");
    const auto& step = episode.steps[ordinal];
    std::vector<SessionEncodedClaim> claims;
    std::vector<std::string> unresolved{"semantic_source_binding_unresolved"};
    std::shared_ptr<const SessionContentEncoding> encoding;
    try {
        const auto bound = restore_session_semantics(
            step.observation, source_memory, source_episodes);
        if (ordinal != 0 || !same_episode(bound->derivative, episode))
            throw std::invalid_argument("session derivative lineage changed");
        encoding = std::make_shared<const SessionContentEncoding>(bound);
        const auto graph = claim_graph_addresses(*bound);
        claims.reserve(bound->units.size());
        for (std::size_t index = 0; index != bound->units.size(); ++index) {
            const auto& row = bound->units[index];
            std::vector<std::string> referenced;
            referenced.reserve(row.anchors.size());
            for (const auto& anchor : row.anchors) referenced.push_back(anchor.identifier);
            const auto other = bound->unresolved.size() >= row.unresolved_anchors.size()
                ? bound->unresolved.size() - row.unresolved_anchors.size() : 0;
            claims.push_back({{episode.episode_id, ordinal, index}, row.unit.subject,
                row.unit.predicate, row.unit.value, row.unit, proposition_key(row.unit),
                graph.at(index), {std::move(referenced), row.unresolved_anchors, other,
                                  row.input_context, bound->document_key}});
        }
        unresolved.clear();
        for (const auto& anchor : bound->unresolved)
            unresolved.push_back("semantic_anchor_unresolved:" + anchor);
    } catch (const std::invalid_argument&) {
        claims.clear();
        encoding.reset();
        unresolved = {"semantic_source_binding_unresolved"};
    }
    return {episode.episode_id, ordinal, episode.episode_id,
        "source_bound_session_semantic_proposal", "semantic_interpretation_proposal",
        std::move(claims), episode.source_addresses, step.evidence_refs, episode.revision,
        step.outcome, episode.verification_state, std::move(unresolved), std::move(encoding)};
}

}  // namespace swegca::world
