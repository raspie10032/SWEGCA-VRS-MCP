#pragma once

#include "world/session_document_directory.hpp"
#include "world/session_occurrences.hpp"

#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view prepared_session_cache_source_sha256 =
    "365499068b514ec2c5db0bbedf4ce42905ac8358a59401a11dcd247788828402";

class PreparedSessionEntry final {
public:
    PreparedSessionEntry(SessionDocument document,
        std::vector<SessionDocumentUnresolvedStep> unresolved_steps,
        std::size_t maximum_document_bytes,
        SessionOccurrenceIndex occurrence_index);

    const SessionDocument document;
    const std::vector<SessionDocumentUnresolvedStep> unresolved_steps;
    const std::size_t maximum_document_bytes;
    const SessionOccurrenceIndex occurrence_index;

    [[nodiscard]] std::string_view status() const noexcept { return document.status; }
    [[nodiscard]] constexpr bool grants_authority() const noexcept { return false; }
    [[nodiscard]] constexpr bool semantic_content_completed() const noexcept { return false; }
};

using PreparedSessionEntryPtr = std::shared_ptr<const PreparedSessionEntry>;
using PreparedSessionChange = std::optional<PreparedSessionEntryPtr>;

struct PreparedSessionCacheLayer final {
    std::map<SessionDocumentKey, PreparedSessionChange> values;
    std::size_t weight{};
    std::map<SessionCallKey, std::vector<SessionDocumentKey>> call_documents;
    std::map<SessionSourcePosition, std::vector<SessionDocumentKey>> position_documents;
};

class PreparedSessionView;

class PreparedSessionCache final {
public:
    PreparedSessionCache() = default;
    explicit PreparedSessionCache(std::vector<PreparedSessionCacheLayer> layers);

    [[nodiscard]] PreparedSessionEntryPtr get(const SessionDocumentKey& key) const;
    [[nodiscard]] PreparedSessionCache with_changes(
        const std::map<SessionDocumentKey, PreparedSessionChange>& changes) const;
    [[nodiscard]] PreparedSessionCache invalidate(
        const std::vector<SessionDocumentKey>& changed_keys) const;
    [[nodiscard]] std::vector<SessionDocumentKey> documents_for_call(
        const SessionCallKey& key) const;
    [[nodiscard]] std::vector<SessionDocumentKey> documents_for_position(
        const SessionSourcePosition& key) const;
    [[nodiscard]] PreparedSessionView bind(
        std::string memory_snapshot_id, SessionDocumentDirectoryView directory) const;
    [[nodiscard]] const std::vector<PreparedSessionCacheLayer>& layers() const noexcept;

private:
    std::vector<PreparedSessionCacheLayer> layers_;
};

class PreparedSessionView final {
public:
    PreparedSessionView(std::string memory_snapshot_id, PreparedSessionCache cache,
                        SessionDocumentDirectoryView directory);

    const std::string memory_snapshot_id;
    const PreparedSessionCache cache;
    const SessionDocumentDirectoryView directory;

    [[nodiscard]] constexpr bool grants_authority() const noexcept { return false; }
    [[nodiscard]] PreparedSessionEntryPtr get(
        const SessionDocumentKey& key, std::string_view expected_memory_snapshot_id) const;
    [[nodiscard]] std::vector<std::pair<SessionDocumentKey, PreparedSessionEntryPtr>> for_episode(
        std::string_view identifier, std::string_view expected_memory_snapshot_id) const;
    [[nodiscard]] SessionCallJoin call_join(
        const SessionCallKey& key, std::string_view expected_memory_snapshot_id) const;
};

[[nodiscard]] PreparedSessionCache prepare_session_cache_document(
    const std::vector<SemanticSourceEpisode>& episodes,
    const SessionDocumentDirectoryView& directory,
    std::string memory_snapshot_id,
    const PreparedSessionCache& cache,
    const SessionDocumentKey& key,
    std::size_t maximum_document_bytes = 16U * 1024U * 1024U);

}  // namespace swegca::world
