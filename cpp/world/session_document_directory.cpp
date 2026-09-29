#include "world/session_document_directory.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <utility>

namespace swegca::world {
namespace {

[[nodiscard]] SessionDocumentDirectoryShard merge(
    SessionDocumentDirectoryShard left, const SessionDocumentDirectoryShard& right) {
    for (const auto& key : right.document_order)
        if (std::ranges::find(left.document_order, key) == left.document_order.end())
            left.document_order.push_back(key);
    for (const auto& [key, rows] : right.by_document) {
        auto& target = left.by_document[key];
        target.insert(target.end(), rows.begin(), rows.end());
    }
    for (const auto& [key, rows] : right.by_episode) {
        auto& target = left.by_episode[key];
        target.insert(target.end(), rows.begin(), rows.end());
    }
    for (const auto& [key, rows] : right.unresolved) {
        auto& target = left.unresolved[key];
        target.insert(target.end(), rows.begin(), rows.end());
    }
    left.size += right.size;
    return left;
}

void check_generation(const std::string_view expected, const std::string_view actual) {
    if (expected != actual)
        throw std::invalid_argument("session directory memory generation changed");
}

}  // namespace

void DocumentDirectoryBuilder::observe(
    std::string identifier, const std::size_t step, const JsonValue& observation) {
    const auto binding = session_document_binding(observation);
    if (binding.kind == SessionDocumentBinding::Kind::unrelated) return;
    ++shard_.size;
    if (binding.kind == SessionDocumentBinding::Kind::unresolved) {
        shard_.unresolved[std::move(identifier)].push_back({step, binding.reason});
        return;
    }
    DocumentFragmentAddress reference{binding.document_key, std::move(identifier),
                                      step, binding.character_offset};
    if (!shard_.by_document.contains(binding.document_key))
        shard_.document_order.push_back(binding.document_key);
    shard_.by_document[binding.document_key].push_back(reference);
    shard_.by_episode[reference.episode_id].push_back(std::move(reference));
}

SessionDocumentDirectoryShard DocumentDirectoryBuilder::finish() {
    return std::move(shard_);
}

SessionDocumentDirectoryView::SessionDocumentDirectoryView(
    std::string memory_snapshot_id_value,
    std::vector<SessionDocumentDirectoryShard> shards_value)
    : memory_snapshot_id(std::move(memory_snapshot_id_value)),
      shards(std::move(shards_value)) {}

std::vector<SessionDocumentKey> SessionDocumentDirectoryView::keys_for_episode(
    const std::string_view identifier,
    const std::string_view expected_memory_snapshot_id) const {
    check_generation(expected_memory_snapshot_id, memory_snapshot_id);
    std::vector<SessionDocumentKey> result;
    std::set<SessionDocumentKey> seen;
    for (const auto& shard : shards)
        if (const auto found = shard.by_episode.find(identifier); found != shard.by_episode.end())
            for (const auto& reference : found->second)
                if (seen.insert(reference.document_key).second)
                    result.push_back(reference.document_key);
    return result;
}

std::vector<SessionDocumentKey> SessionDocumentDirectoryView::keys_for_step(
    const std::string_view identifier, const std::size_t step,
    const std::string_view expected_memory_snapshot_id) const {
    check_generation(expected_memory_snapshot_id, memory_snapshot_id);
    std::vector<SessionDocumentKey> result;
    std::set<SessionDocumentKey> seen;
    for (const auto& shard : shards)
        if (const auto found = shard.by_episode.find(identifier); found != shard.by_episode.end())
            for (const auto& reference : found->second)
                if (reference.step == step && seen.insert(reference.document_key).second)
                    result.push_back(reference.document_key);
    return result;
}

std::vector<DocumentFragmentAddress> SessionDocumentDirectoryView::fragments(
    const SessionDocumentKey& key,
    const std::string_view expected_memory_snapshot_id) const {
    check_generation(expected_memory_snapshot_id, memory_snapshot_id);
    std::vector<DocumentFragmentAddress> result;
    for (const auto& shard : shards)
        if (const auto found = shard.by_document.find(key); found != shard.by_document.end())
            result.insert(result.end(), found->second.begin(), found->second.end());
    return result;
}

std::vector<SessionDirectoryUnresolved> SessionDocumentDirectoryView::unresolved_for_episode(
    const std::string_view identifier,
    const std::string_view expected_memory_snapshot_id) const {
    check_generation(expected_memory_snapshot_id, memory_snapshot_id);
    std::vector<SessionDirectoryUnresolved> result;
    for (const auto& shard : shards)
        if (const auto found = shard.unresolved.find(identifier); found != shard.unresolved.end())
            result.insert(result.end(), found->second.begin(), found->second.end());
    return result;
}

SessionDocumentDirectory::SessionDocumentDirectory(
    std::vector<SessionDocumentDirectoryShard> shards)
    : shards_(std::move(shards)) {}

SessionDocumentDirectory SessionDocumentDirectory::append_shard(
    SessionDocumentDirectoryShard added) const {
    if (!added.size) return *this;
    auto shards = shards_;
    shards.push_back(std::move(added));
    while (shards.size() > 1 && 2 * shards.back().size >= shards[shards.size() - 2].size) {
        auto combined = merge(std::move(shards[shards.size() - 2]), shards.back());
        shards.pop_back();
        shards.back() = std::move(combined);
    }
    return SessionDocumentDirectory(std::move(shards));
}

SessionDocumentDirectory SessionDocumentDirectory::append(
    const std::vector<SemanticSourceEpisode>& episodes) const {
    return append_with_keys(episodes).first;
}

std::pair<SessionDocumentDirectory, std::vector<SessionDocumentKey>>
SessionDocumentDirectory::append_with_keys(
    const std::vector<SemanticSourceEpisode>& episodes) const {
    DocumentDirectoryBuilder builder;
    for (const auto& episode : episodes)
        for (std::size_t ordinal = 0; ordinal != episode.steps.size(); ++ordinal)
            builder.observe(episode.episode_id, ordinal, episode.steps[ordinal].observation);
    auto shard = builder.finish();
    std::vector<SessionDocumentKey> keys;
    keys = shard.document_order;
    return {append_shard(std::move(shard)), std::move(keys)};
}

SessionDocumentDirectoryView SessionDocumentDirectory::bind(
    std::string memory_snapshot_id) const {
    return {std::move(memory_snapshot_id), shards_};
}

const std::vector<SessionDocumentDirectoryShard>& SessionDocumentDirectory::shards() const noexcept {
    return shards_;
}

SessionDocumentPreparation prepare_indexed_session_document(
    const std::vector<SemanticSourceEpisode>& episodes,
    const SessionDocumentDirectoryView& directory,
    const SessionDocumentKey& key,
    std::string memory_snapshot_id,
    const std::size_t maximum_document_bytes) {
    const auto references = directory.fragments(key, memory_snapshot_id);
    if (references.empty()) throw std::out_of_range("session document key not indexed");
    std::vector<std::string> identifiers;
    for (const auto& reference : references)
        if (std::ranges::find(identifiers, reference.episode_id) == identifiers.end())
            identifiers.push_back(reference.episode_id);
    std::vector<SemanticSourceEpisode> selected;
    for (const auto& identifier : identifiers) {
        const auto episode = std::ranges::find_if(episodes, [&](const auto& candidate) {
            return candidate.episode_id == identifier;
        });
        if (episode != episodes.end()) selected.push_back(*episode);
    }
    if (selected.size() != identifiers.size())
        throw std::invalid_argument("indexed session parent unavailable");
    return prepare_session_documents(selected, std::move(memory_snapshot_id),
                                     maximum_document_bytes, key);
}

}  // namespace swegca::world
