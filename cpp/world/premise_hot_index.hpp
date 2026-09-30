#pragma once

#include "world/atom_hot_index.hpp"
#include "world/prepared_session_cache.hpp"
#include "world/recorded_claims.hpp"
#include "world/semantic_family_directory.hpp"
#include "world/syllogism_sources.hpp"

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view premise_hot_index_source_sha256 =
    "4ddb3f691265abec89da20374c1a2efb65337b7b0cb6e46831f799311e1e4ec6";

class PremiseHotMemoryIndex final : public HotMemoryIndex,
                                    public std::enable_shared_from_this<PremiseHotMemoryIndex> {
public:
    struct Shard;

    [[nodiscard]] static std::shared_ptr<const PremiseHotMemoryIndex> build(
        std::shared_ptr<const HotMemoryIndex> base,
        const std::vector<std::string>& prepared_episode_ids = {});

    [[nodiscard]] std::string_view snapshot_id() const noexcept override;
    [[nodiscard]] std::size_t episode_count() const noexcept override;
    [[nodiscard]] const std::map<std::string, std::size_t, std::less<>>&
        outcome_counts() const noexcept override;
    [[nodiscard]] const MemoryEpisode& episode(std::string_view episode_id) const override;
    [[nodiscard]] std::vector<std::string> episode_ids_for_cue(
        std::string_view cue) const override;
    [[nodiscard]] std::vector<std::string> iter_episode_ids() const override;
    [[nodiscard]] std::vector<SemanticFamilyDirectory>
        semantic_family_directories() const override;

    [[nodiscard]] std::string preparation_state(std::string_view identifier) const;
    [[nodiscard]] PremiseReadView premise_view() const;
    [[nodiscard]] std::optional<RecordedEncodedEvent> recorded_event(
        std::string_view identifier, std::size_t step) const;
    [[nodiscard]] SessionDocumentDirectoryView session_document_view() const;
    [[nodiscard]] PreparedSessionView prepared_session_view() const;
    [[nodiscard]] std::shared_ptr<const PremiseHotMemoryIndex> append(
        const std::vector<MemoryEpisode>& episodes,
        const std::vector<std::string>& required_outcomes = {}) const;
    [[nodiscard]] std::pair<std::shared_ptr<const PremiseHotMemoryIndex>, std::size_t>
        after_vrs_rebind(std::shared_ptr<const HotMemoryIndex> replacement) const;

    const std::shared_ptr<const HotMemoryIndex> base;
    const std::vector<std::shared_ptr<const Shard>> shards;
    const SessionDocumentDirectory session_documents;
    const PreparedSessionCache prepared_sessions;

private:
    PremiseHotMemoryIndex(std::shared_ptr<const HotMemoryIndex> base,
        std::vector<std::shared_ptr<const Shard>> shards,
        SessionDocumentDirectory session_documents,
        PreparedSessionCache prepared_sessions);
};

}  // namespace swegca::world
