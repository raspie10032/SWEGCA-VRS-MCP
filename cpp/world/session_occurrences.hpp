#pragma once

#include "world/session_document.hpp"

#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view session_occurrences_source_sha256 =
    "08c2fb3169ecb9e356c05e41be8101e212f1837480d8c1f9211e183e9a675d5b";

struct OccurrenceReference final {
    std::string episode_id;
    std::size_t step{};
    std::size_t variant{};
    std::size_t occurrence_index{};
    friend bool operator==(const OccurrenceReference&, const OccurrenceReference&) = default;
};

struct SessionCallKey final {
    std::string session;
    std::string turn;
    std::string family;
    std::string call_id;
    friend auto operator<=>(const SessionCallKey&, const SessionCallKey&) = default;
};

struct SessionSourcePosition final {
    std::string path;
    std::string sha256;
    std::size_t byte_boundary{};
    std::size_t line{};
    std::size_t offset{};
    std::size_t bytes{};
    friend auto operator<=>(const SessionSourcePosition&, const SessionSourcePosition&) = default;
};

struct BoundSessionOccurrence final {
    std::shared_ptr<const PreparedSessionArchive> archive;
    std::size_t event_ordinal{};
    std::string session;
    std::string turn;
    std::optional<SessionCallKey> call_key;
    std::optional<std::string> role;
    SessionSourcePosition source_position;
    std::string source_claim;
    JsonValue metadata;
    std::vector<OccurrenceReference> references;

    [[nodiscard]] const SessionEvent& event() const;
    [[nodiscard]] constexpr bool grants_authority() const noexcept { return false; }
};

struct UnresolvedOccurrence final {
    std::string reason;
    std::optional<OccurrenceReference> reference;
    std::optional<std::size_t> event_ordinal;
};

struct SessionOccurrenceIndex final {
    std::vector<BoundSessionOccurrence> occurrences;
    std::map<SessionCallKey, std::vector<std::size_t>> by_call;
    std::map<std::size_t, std::vector<std::size_t>> by_event;
    std::map<SessionSourcePosition, std::vector<std::size_t>> by_position;
    std::vector<UnresolvedOccurrence> unresolved;

    [[nodiscard]] constexpr bool grants_authority() const noexcept { return false; }
};

[[nodiscard]] SessionOccurrenceIndex prepare_session_occurrences(
    const SessionDocument* document);

struct SessionCallJoin final {
    std::string memory_snapshot_id;
    SessionCallKey key;
    std::vector<std::vector<const BoundSessionOccurrence*>> calls;
    std::vector<std::vector<const BoundSessionOccurrence*>> results;
    std::string status;
    std::string scope{"prepared_documents_not_all_experience"};

    [[nodiscard]] constexpr bool causal_or_world_success_verified() const noexcept { return false; }
    [[nodiscard]] constexpr bool grants_authority() const noexcept { return false; }
};

[[nodiscard]] SessionCallJoin join_session_call(
    const SessionCallKey& key,
    const std::vector<const BoundSessionOccurrence*>& occurrences,
    std::string memory_snapshot_id,
    const std::vector<const BoundSessionOccurrence*>& position_witnesses = {});

}  // namespace swegca::world
