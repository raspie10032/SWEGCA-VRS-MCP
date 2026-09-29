#pragma once

#include "world/semantic_vrs_ingress.hpp"
#include "world/session_event_index.hpp"

#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view session_document_source_sha256 =
    "8f4a09fc6af97dcdd02c1742a154a8e9af51b88dab72ee7084f1b3280a712b08";
inline constexpr std::string_view authored_media_observation_schema =
    "rozephine-authored-media-observation-v1";

using SessionDocumentKey = std::pair<std::string, std::string>;

struct SessionDocumentBinding final {
    enum class Kind : unsigned char { unrelated, unresolved, bound };
    Kind kind{Kind::unrelated};
    SessionDocumentKey document_key;
    std::size_t character_offset{};
    std::string reason;
};

[[nodiscard]] SessionDocumentBinding session_document_binding(const JsonValue& observation);

struct SessionFragment final {
    std::string episode_id;
    std::string revision;
    std::size_t step{};
    std::size_t variant{};
    std::size_t character_offset{};
    std::string text;
    std::vector<std::string> source_addresses;
    std::string outcome;
    JsonValue historical_provenance;
};

struct SessionCharacterRange final {
    std::size_t begin{};
    std::size_t end{};
    friend bool operator==(const SessionCharacterRange&, const SessionCharacterRange&) = default;
};

struct SessionDocument final {
    std::string document_id;
    std::string declared_sha256;
    std::vector<SessionFragment> fragments;
    std::string status;
    std::shared_ptr<const PreparedSessionArchive> archive;
    std::vector<SessionCharacterRange> missing_character_ranges;

    [[nodiscard]] constexpr bool grants_authority() const noexcept { return false; }
    [[nodiscard]] constexpr bool semantic_content_completed() const noexcept { return false; }
};

struct SessionDocumentUnresolvedStep final {
    std::string episode_id;
    std::size_t step{};
    std::string reason;
};

class SessionDocumentPreparation final {
public:
    SessionDocumentPreparation(std::string memory_snapshot_id,
        std::map<SessionDocumentKey, SessionDocument> documents,
        std::map<std::string, std::vector<SessionDocumentKey>, std::less<>> by_episode,
        std::vector<SessionDocumentUnresolvedStep> unresolved_steps);

    const std::string memory_snapshot_id;
    const std::map<SessionDocumentKey, SessionDocument> documents;
    const std::map<std::string, std::vector<SessionDocumentKey>, std::less<>> by_episode;
    const std::vector<SessionDocumentUnresolvedStep> unresolved_steps;

    [[nodiscard]] constexpr std::size_t new_experience_count() const noexcept { return 0; }
    [[nodiscard]] std::vector<const SessionDocument*> for_episode(
        std::string_view episode_id, std::string_view expected_memory_snapshot_id) const;
};

[[nodiscard]] SessionDocumentPreparation prepare_session_documents(
    const std::vector<SemanticSourceEpisode>& episodes,
    std::string memory_snapshot_id,
    std::size_t maximum_document_bytes = 16U * 1024U * 1024U,
    std::optional<SessionDocumentKey> target_document_key = std::nullopt);

}  // namespace swegca::world
