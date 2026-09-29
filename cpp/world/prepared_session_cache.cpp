#include "world/prepared_session_cache.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <utility>

namespace swegca::world {
namespace {

template<class Key>
void replace_document(std::map<Key, std::vector<SessionDocumentKey>>& updates,
                      const Key& index_key, const SessionDocumentKey& document_key,
                      const bool present, const std::vector<SessionDocumentKey>& existing) {
    auto [found, inserted] = updates.emplace(index_key, existing);
    auto& documents = found->second;
    documents.erase(std::remove(documents.begin(), documents.end(), document_key), documents.end());
    if (present && std::ranges::find(documents, document_key) == documents.end())
        documents.push_back(document_key);
    std::ranges::sort(documents);
}

[[nodiscard]] PreparedSessionCacheLayer merge(
    PreparedSessionCacheLayer old, PreparedSessionCacheLayer newest) {
    for (auto& [key, value] : newest.values) old.values[key] = std::move(value);
    for (auto& [key, value] : newest.call_documents)
        old.call_documents[key] = std::move(value);
    for (auto& [key, value] : newest.position_documents)
        old.position_documents[key] = std::move(value);
    old.weight += newest.weight;
    return old;
}

void check_generation(const std::string_view expected, const std::string_view actual) {
    if (expected != actual)
        throw std::invalid_argument("prepared session memory generation changed");
}

}  // namespace

PreparedSessionEntry::PreparedSessionEntry(
    SessionDocument document_value,
    std::vector<SessionDocumentUnresolvedStep> unresolved_steps_value,
    const std::size_t maximum_document_bytes_value,
    SessionOccurrenceIndex occurrence_index_value)
    : document(std::move(document_value)),
      unresolved_steps(std::move(unresolved_steps_value)),
      maximum_document_bytes(maximum_document_bytes_value),
      occurrence_index(std::move(occurrence_index_value)) {}

PreparedSessionCache::PreparedSessionCache(
    std::vector<PreparedSessionCacheLayer> layers)
    : layers_(std::move(layers)) {}

PreparedSessionEntryPtr PreparedSessionCache::get(const SessionDocumentKey& key) const {
    for (auto layer = layers_.rbegin(); layer != layers_.rend(); ++layer)
        if (const auto found = layer->values.find(key); found != layer->values.end())
            return found->second.value_or(nullptr);
    return nullptr;
}

std::vector<SessionDocumentKey> PreparedSessionCache::documents_for_call(
    const SessionCallKey& key) const {
    for (auto layer = layers_.rbegin(); layer != layers_.rend(); ++layer)
        if (const auto found = layer->call_documents.find(key); found != layer->call_documents.end())
            return found->second;
    return {};
}

std::vector<SessionDocumentKey> PreparedSessionCache::documents_for_position(
    const SessionSourcePosition& key) const {
    for (auto layer = layers_.rbegin(); layer != layers_.rend(); ++layer)
        if (const auto found = layer->position_documents.find(key);
            found != layer->position_documents.end()) return found->second;
    return {};
}

PreparedSessionCache PreparedSessionCache::with_changes(
    const std::map<SessionDocumentKey, PreparedSessionChange>& changes) const {
    if (changes.empty()) return *this;
    PreparedSessionCacheLayer added;
    added.values = changes;
    added.weight = changes.size();
    for (const auto& [document_key, change] : changes) {
        const auto previous = get(document_key);
        std::set<SessionCallKey> call_keys;
        std::set<SessionSourcePosition> position_keys;
        if (previous) {
            for (const auto& [key, unused] : previous->occurrence_index.by_call) {
                (void)unused; call_keys.insert(key);
            }
            for (const auto& [key, unused] : previous->occurrence_index.by_position) {
                (void)unused; position_keys.insert(key);
            }
        }
        const auto next = change.value_or(nullptr);
        if (next) {
            for (const auto& [key, unused] : next->occurrence_index.by_call) {
                (void)unused; call_keys.insert(key);
            }
            for (const auto& [key, unused] : next->occurrence_index.by_position) {
                (void)unused; position_keys.insert(key);
            }
        }
        for (const auto& key : call_keys)
            replace_document(added.call_documents, key, document_key,
                next && next->occurrence_index.by_call.contains(key), documents_for_call(key));
        for (const auto& key : position_keys)
            replace_document(added.position_documents, key, document_key,
                next && next->occurrence_index.by_position.contains(key), documents_for_position(key));
    }
    auto layers = layers_;
    layers.push_back(std::move(added));
    while (layers.size() > 1 &&
           2 * layers.back().weight >= layers[layers.size() - 2].weight) {
        auto combined = merge(std::move(layers[layers.size() - 2]), std::move(layers.back()));
        layers.pop_back();
        layers.back() = std::move(combined);
    }
    return PreparedSessionCache(std::move(layers));
}

PreparedSessionCache PreparedSessionCache::invalidate(
    const std::vector<SessionDocumentKey>& changed_keys) const {
    std::map<SessionDocumentKey, PreparedSessionChange> changes;
    for (const auto& key : changed_keys)
        if (get(key)) changes.emplace(key, std::nullopt);
    return with_changes(changes);
}

const std::vector<PreparedSessionCacheLayer>& PreparedSessionCache::layers() const noexcept {
    return layers_;
}

PreparedSessionView::PreparedSessionView(
    std::string memory_snapshot_id_value, PreparedSessionCache cache_value,
    SessionDocumentDirectoryView directory_value)
    : memory_snapshot_id(std::move(memory_snapshot_id_value)),
      cache(std::move(cache_value)), directory(std::move(directory_value)) {}

PreparedSessionEntryPtr PreparedSessionView::get(
    const SessionDocumentKey& key,
    const std::string_view expected_memory_snapshot_id) const {
    check_generation(expected_memory_snapshot_id, memory_snapshot_id);
    return cache.get(key);
}

std::vector<std::pair<SessionDocumentKey, PreparedSessionEntryPtr>>
PreparedSessionView::for_episode(
    const std::string_view identifier,
    const std::string_view expected_memory_snapshot_id) const {
    check_generation(expected_memory_snapshot_id, memory_snapshot_id);
    std::vector<std::pair<SessionDocumentKey, PreparedSessionEntryPtr>> result;
    for (const auto& key : directory.keys_for_episode(identifier, memory_snapshot_id))
        result.emplace_back(key, cache.get(key));
    return result;
}

SessionCallJoin PreparedSessionView::call_join(
    const SessionCallKey& key,
    const std::string_view expected_memory_snapshot_id) const {
    check_generation(expected_memory_snapshot_id, memory_snapshot_id);
    if (key.session.empty() || key.turn.empty() || key.call_id.empty() ||
        (key.family != "function" && key.family != "custom"))
        throw std::invalid_argument("explicit session turn family call ID required");
    std::vector<const BoundSessionOccurrence*> selected;
    for (const auto& document_key : cache.documents_for_call(key)) {
        const auto entry = cache.get(document_key);
        if (!entry) throw std::logic_error("prepared session call directory inconsistent");
        const auto found = entry->occurrence_index.by_call.find(key);
        if (found == entry->occurrence_index.by_call.end())
            throw std::logic_error("prepared session call directory inconsistent");
        for (const auto index : found->second)
            selected.push_back(&entry->occurrence_index.occurrences.at(index));
    }
    std::vector<const BoundSessionOccurrence*> witnesses;
    std::set<SessionSourcePosition> positions;
    for (const auto* occurrence : selected) positions.insert(occurrence->source_position);
    for (const auto& position : positions)
        for (const auto& document_key : cache.documents_for_position(position)) {
            const auto entry = cache.get(document_key);
            if (!entry) throw std::logic_error("prepared session position directory inconsistent");
            const auto found = entry->occurrence_index.by_position.find(position);
            if (found == entry->occurrence_index.by_position.end())
                throw std::logic_error("prepared session position directory inconsistent");
            for (const auto index : found->second)
                witnesses.push_back(&entry->occurrence_index.occurrences.at(index));
        }
    return join_session_call(key, selected, memory_snapshot_id, witnesses);
}

PreparedSessionCache prepare_session_cache_document(
    const std::vector<SemanticSourceEpisode>& episodes,
    const SessionDocumentDirectoryView& directory,
    std::string memory_snapshot_id,
    const PreparedSessionCache& cache,
    const SessionDocumentKey& key,
    const std::size_t maximum_document_bytes) {
    if (maximum_document_bytes == 0)
        throw std::invalid_argument("positive document preparation capacity required");
    const auto previous = cache.get(key);
    if (previous && !(previous->status() == "capacity_deferred" &&
                      maximum_document_bytes > previous->maximum_document_bytes))
        return cache;
    auto result = prepare_indexed_session_document(
        episodes, directory, key, std::move(memory_snapshot_id), maximum_document_bytes);
    const auto found = result.documents.find(key);
    if (found == result.documents.end())
        throw std::logic_error("indexed session document was not reconstructed");
    auto document = found->second;
    auto occurrences = prepare_session_occurrences(&document);
    auto entry = std::make_shared<PreparedSessionEntry>(
        std::move(document), result.unresolved_steps,
        maximum_document_bytes, std::move(occurrences));
    return cache.with_changes({{key, entry}});
}

}  // namespace swegca::world
