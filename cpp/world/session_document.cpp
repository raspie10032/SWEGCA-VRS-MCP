#include "world/session_document.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace swegca::world {
namespace {

[[nodiscard]] const JsonValue* field(const JsonValue& value, const std::string_view key) {
    if (!value.is_object()) return nullptr;
    const auto found = value.as_object().find(key);
    return found == value.as_object().end() ? nullptr : &found->second;
}

[[nodiscard]] std::optional<std::string_view> string_field(
    const JsonValue& value, const std::string_view key) {
    const auto* child = field(value, key);
    if (!child || !std::holds_alternative<std::string>(child->storage())) return std::nullopt;
    return std::get<std::string>(child->storage());
}

[[nodiscard]] std::optional<std::int64_t> integer_field(
    const JsonValue& value, const std::string_view key) {
    const auto* child = field(value, key);
    if (!child || !std::holds_alternative<std::int64_t>(child->storage())) return std::nullopt;
    return std::get<std::int64_t>(child->storage());
}

[[nodiscard]] bool exact_bool(const JsonValue* value, const bool expected) noexcept {
    return value && std::holds_alternative<bool>(value->storage()) &&
        std::get<bool>(value->storage()) == expected;
}

[[nodiscard]] bool truthy(const JsonValue* value) noexcept {
    if (!value || std::holds_alternative<std::nullptr_t>(value->storage())) return false;
    if (const auto* boolean = std::get_if<bool>(&value->storage())) return *boolean;
    if (const auto* integer = std::get_if<std::int64_t>(&value->storage())) return *integer != 0;
    if (const auto* number = std::get_if<double>(&value->storage())) return *number != 0;
    if (const auto* text = std::get_if<std::string>(&value->storage())) return !text->empty();
    if (const auto* array = std::get_if<JsonValue::Array>(&value->storage())) return !array->empty();
    return !std::get<JsonValue::Object>(value->storage()).empty();
}

[[nodiscard]] bool digest_text(const std::string_view value) noexcept {
    return value.size() == 64 && std::ranges::all_of(value, [](const unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

[[nodiscard]] std::size_t unicode_length(const std::string_view value) {
    std::size_t count = 0;
    for (std::size_t at = 0; at != value.size(); ++count) {
        const auto first = static_cast<unsigned char>(value[at]);
        const auto size = first < 0x80U ? 1U : (first & 0xe0U) == 0xc0U ? 2U :
            (first & 0xf0U) == 0xe0U ? 3U : (first & 0xf8U) == 0xf0U ? 4U : 0U;
        if (!size || size > value.size() - at) throw std::invalid_argument("invalid document UTF-8");
        for (std::size_t index = 1; index != size; ++index)
            if ((static_cast<unsigned char>(value[at + index]) & 0xc0U) != 0x80U)
                throw std::invalid_argument("invalid document UTF-8");
        at += size;
    }
    return count;
}

[[nodiscard]] SessionDocument assemble(
    const SessionDocumentKey& key, std::vector<SessionFragment> fragments,
    const std::size_t maximum_document_bytes) {
    std::map<std::size_t, std::string> at_offset;
    for (const auto& fragment : fragments) {
        const auto [found, inserted] = at_offset.emplace(fragment.character_offset, fragment.text);
        if (!inserted && found->second != fragment.text)
            return {key.first, key.second, std::move(fragments), "conflicting_fragments", nullptr, {}};
    }
    std::size_t cursor = 0;
    std::size_t logical_bytes = 0;
    std::vector<SessionCharacterRange> missing;
    std::string text;
    for (const auto& [offset, fragment] : at_offset) {
        if (offset < cursor)
            return {key.first, key.second, std::move(fragments), "overlapping_fragments", nullptr, {}};
        if (offset > cursor) missing.push_back({cursor, offset});
        cursor = offset + unicode_length(fragment);
        if (fragment.size() > maximum_document_bytes - std::min(maximum_document_bytes, logical_bytes))
            return {key.first, key.second, std::move(fragments), "capacity_deferred", nullptr, {}};
        logical_bytes += fragment.size();
        if (logical_bytes > maximum_document_bytes)
            return {key.first, key.second, std::move(fragments), "capacity_deferred", nullptr, {}};
        text += fragment;
    }
    if (!missing.empty())
        return {key.first, key.second, std::move(fragments), "missing_fragments", nullptr,
                std::move(missing)};
    try {
        auto archive = prepare_session_archive(
            std::move(text), key.first, key.second, key.second);
        if (!archive)
            return {key.first, key.second, std::move(fragments), "opaque_document", nullptr, {}};
        return {key.first, key.second, std::move(fragments), "prepared",
                std::make_shared<PreparedSessionArchive>(std::move(*archive)), {}};
    } catch (const SessionDocumentDigestMismatch&) {
        return {key.first, key.second, std::move(fragments), "document_digest_mismatch", nullptr, {}};
    }
}

}  // namespace

SessionDocumentBinding session_document_binding(const JsonValue& observation) {
    const auto source_type = string_field(observation, "source_type");
    if (!source_type || *source_type != "codex_session_record") return {};
    const auto schema = string_field(observation, "schema_version");
    const auto* content = field(observation, "content");
    if (!schema || *schema != authored_media_observation_schema || !content || !content->is_object())
        return {SessionDocumentBinding::Kind::unresolved, {}, 0, "unknown_session_observation"};
    const auto* document = field(*content, "source_document");
    const auto* provenance = field(*content, "historical_provenance");
    if (!document || !document->is_object() || !provenance || !provenance->is_object())
        return {SessionDocumentBinding::Kind::unresolved, {}, 0, "incomplete_document_binding"};
    const auto identity = string_field(*document, "identity");
    const auto digest = string_field(*document, "sha256");
    const auto offset = integer_field(*provenance, "character_offset");
    if (!identity || identity->empty() || !digest || !digest_text(*digest) ||
        !exact_bool(field(*document, "fragments_are_independent_outcomes"), false) ||
        !truthy(field(*document, "license")) ||
        !exact_bool(field(*provenance, "duplicate_occurrences_are_not_new_events"), true) ||
        !exact_bool(field(*provenance, "semantics_and_actual_world_success_not_verified"), true) ||
        !offset || *offset < 0)
        return {SessionDocumentBinding::Kind::unresolved, {}, 0, "incomplete_document_binding"};
    return {SessionDocumentBinding::Kind::bound,
            {std::string(*identity), std::string(*digest)}, static_cast<std::size_t>(*offset), {}};
}

SessionDocumentPreparation::SessionDocumentPreparation(
    std::string memory_snapshot_id_value,
    std::map<SessionDocumentKey, SessionDocument> documents_value,
    std::map<std::string, std::vector<SessionDocumentKey>, std::less<>> by_episode_value,
    std::vector<SessionDocumentUnresolvedStep> unresolved_steps_value)
    : memory_snapshot_id(std::move(memory_snapshot_id_value)),
      documents(std::move(documents_value)), by_episode(std::move(by_episode_value)),
      unresolved_steps(std::move(unresolved_steps_value)) {}

std::vector<const SessionDocument*> SessionDocumentPreparation::for_episode(
    const std::string_view episode_id,
    const std::string_view expected_memory_snapshot_id) const {
    if (expected_memory_snapshot_id != memory_snapshot_id)
        throw std::invalid_argument("session document memory generation changed");
    std::vector<const SessionDocument*> result;
    if (const auto found = by_episode.find(episode_id); found != by_episode.end())
        for (const auto& key : found->second) result.push_back(&documents.at(key));
    return result;
}

SessionDocumentPreparation prepare_session_documents(
    const std::vector<SemanticSourceEpisode>& episodes, std::string memory_snapshot_id,
    const std::size_t maximum_document_bytes,
    std::optional<SessionDocumentKey> target_document_key) {
    if (memory_snapshot_id.empty() || maximum_document_bytes == 0)
        throw std::invalid_argument("main snapshot and positive preparation capacity required");
    std::map<SessionDocumentKey, std::vector<SessionFragment>> groups;
    std::vector<SessionDocumentUnresolvedStep> unresolved;
    std::set<std::string> seen;
    for (const auto& episode : episodes) {
        if (!seen.insert(episode.episode_id).second)
            throw std::invalid_argument("duplicate parent address in one main snapshot");
        for (std::size_t ordinal = 0; ordinal != episode.steps.size(); ++ordinal) {
            const auto& step = episode.steps[ordinal];
            const auto binding = session_document_binding(step.observation);
            if (binding.kind == SessionDocumentBinding::Kind::unrelated) continue;
            if (binding.kind == SessionDocumentBinding::Kind::unresolved) {
                unresolved.push_back({episode.episode_id, ordinal, binding.reason});
                continue;
            }
            if (target_document_key && binding.document_key != *target_document_key) continue;
            const auto* content = field(step.observation, "content");
            const auto* provenance = content ? field(*content, "historical_provenance") : nullptr;
            const auto* variants = content ? field(*content, "variants") : nullptr;
            if (!provenance || !variants || !variants->is_array() || variants->as_array().empty()) {
                unresolved.push_back({episode.episode_id, ordinal, "incomplete_document_binding"});
                continue;
            }
            for (std::size_t position = 0; position != variants->as_array().size(); ++position) {
                const auto& variant = variants->as_array()[position];
                const auto text_value = string_field(variant, "text");
                if (!variant.is_object() || !text_value || text_value->empty() ||
                    !truthy(field(variant, "source_address"))) {
                    unresolved.push_back({episode.episode_id, ordinal, "unknown_document_variant"});
                    continue;
                }
                groups[binding.document_key].push_back({episode.episode_id, episode.revision,
                    ordinal, position, binding.character_offset, std::string(*text_value),
                    episode.source_addresses, step.outcome, *provenance});
            }
        }
    }
    std::map<SessionDocumentKey, SessionDocument> documents;
    std::map<std::string, std::vector<SessionDocumentKey>, std::less<>> by_episode;
    for (auto& [key, fragments] : groups) {
        for (const auto& fragment : fragments) {
            auto& keys = by_episode[fragment.episode_id];
            if (std::ranges::find(keys, key) == keys.end()) keys.push_back(key);
        }
        documents.emplace(key, assemble(key, std::move(fragments), maximum_document_bytes));
    }
    return {std::move(memory_snapshot_id), std::move(documents), std::move(by_episode),
            std::move(unresolved)};
}

}  // namespace swegca::world
