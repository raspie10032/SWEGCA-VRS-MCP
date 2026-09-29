#pragma once

#include "world/session_document.hpp"

#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view session_document_directory_source_sha256 =
    "0e410a779eaa2083b1fa906bd8970e8fb2e68a9b4b2dc943533f31ba4cf1094c";

struct DocumentFragmentAddress final {
    SessionDocumentKey document_key;
    std::string episode_id;
    std::size_t step{};
    std::size_t character_offset{};
    friend bool operator==(const DocumentFragmentAddress&,
                           const DocumentFragmentAddress&) = default;
};

struct SessionDirectoryUnresolved final {
    std::size_t step{};
    std::string reason;
    friend bool operator==(const SessionDirectoryUnresolved&,
                           const SessionDirectoryUnresolved&) = default;
};

struct SessionDocumentDirectoryShard final {
    std::map<SessionDocumentKey, std::vector<DocumentFragmentAddress>> by_document;
    std::map<std::string, std::vector<DocumentFragmentAddress>, std::less<>> by_episode;
    std::map<std::string, std::vector<SessionDirectoryUnresolved>, std::less<>> unresolved;
    std::size_t size{};
};

class DocumentDirectoryBuilder final {
public:
    void observe(std::string identifier, std::size_t step, const JsonValue& observation);
    [[nodiscard]] SessionDocumentDirectoryShard finish();

private:
    SessionDocumentDirectoryShard shard_;
};

class SessionDocumentDirectoryView final {
public:
    SessionDocumentDirectoryView(std::string memory_snapshot_id,
                                 std::vector<SessionDocumentDirectoryShard> shards);

    const std::string memory_snapshot_id;
    const std::vector<SessionDocumentDirectoryShard> shards;

    [[nodiscard]] constexpr bool grants_authority() const noexcept { return false; }
    [[nodiscard]] constexpr bool semantic_content_completed() const noexcept { return false; }
    [[nodiscard]] std::vector<SessionDocumentKey> keys_for_episode(
        std::string_view identifier, std::string_view expected_memory_snapshot_id) const;
    [[nodiscard]] std::vector<SessionDocumentKey> keys_for_step(
        std::string_view identifier, std::size_t step,
        std::string_view expected_memory_snapshot_id) const;
    [[nodiscard]] std::vector<DocumentFragmentAddress> fragments(
        const SessionDocumentKey& key, std::string_view expected_memory_snapshot_id) const;
    [[nodiscard]] std::vector<SessionDirectoryUnresolved> unresolved_for_episode(
        std::string_view identifier, std::string_view expected_memory_snapshot_id) const;
};

class SessionDocumentDirectory final {
public:
    SessionDocumentDirectory() = default;
    explicit SessionDocumentDirectory(std::vector<SessionDocumentDirectoryShard> shards);

    [[nodiscard]] SessionDocumentDirectory append_shard(SessionDocumentDirectoryShard added) const;
    [[nodiscard]] SessionDocumentDirectory append(
        const std::vector<SemanticSourceEpisode>& episodes) const;
    [[nodiscard]] std::pair<SessionDocumentDirectory, std::vector<SessionDocumentKey>>
        append_with_keys(const std::vector<SemanticSourceEpisode>& episodes) const;
    [[nodiscard]] SessionDocumentDirectoryView bind(std::string memory_snapshot_id) const;
    [[nodiscard]] const std::vector<SessionDocumentDirectoryShard>& shards() const noexcept;

private:
    std::vector<SessionDocumentDirectoryShard> shards_;
};

[[nodiscard]] SessionDocumentPreparation prepare_indexed_session_document(
    const std::vector<SemanticSourceEpisode>& episodes,
    const SessionDocumentDirectoryView& directory,
    const SessionDocumentKey& key,
    std::string memory_snapshot_id,
    std::size_t maximum_document_bytes = 16U * 1024U * 1024U);

}  // namespace swegca::world
