#pragma once

#include "exact_journal_directory.hpp"
#include "hot_index.hpp"
#include "hot_index_projection_log.hpp"
#include "native_cue_directory.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace swegca::vrs {

// The standalone author's ordinary observations have no recorded semantic
// family by default. Explicit family sources remain separate and are never
// derived from shared words or scores. Other HotIndex reads use native projections.
// SWEGCA: src/swegca_vrs2/store.py@7536139:122-175
// SWEGCA: src/swegca_vrs2/engine/mosaic_semantic_family_directory.py@7536139:8-27
class NativePublishedHotIndex final : public PublishedHotIndex {
public:
    // SWEGCA: src/swegca_vrs2/store.py@7536139:122-175
    NativePublishedHotIndex(
        HotIndexSeed memory, std::string pair_snapshot_id,
        std::int64_t published_row_limit,
        std::shared_ptr<const ExactJournalDirectory> originals,
        std::shared_ptr<const HotIndexProjectionLog> headers,
        std::shared_ptr<const NativeCueDirectory> cues,
        std::shared_ptr<const NativeCueDirectory> propositions,
        std::shared_ptr<const NativeCueDirectory> successors,
        std::shared_ptr<const NativeCueDirectory> sources,
        std::vector<std::shared_ptr<const RecordedSemanticFamilyRead>>
            semantic_families = {});

    // SWEGCA: src/swegca_vrs2/store.py@c06092a:1299-1303
    [[nodiscard]] bool has_live_source(std::string_view source) const override;

    // SWEGCA: src/swegca_vrs2/store.py@7536139:146-153
    [[nodiscard]] bool contains_episode(std::string_view identifier) const override;
    // SWEGCA: src/swegca_vrs2/store.py@7536139:129-135
    [[nodiscard]] HotIndexEpisodeHeader episode_header(
        std::string_view identifier) const override;
    // SWEGCA: src/swegca_vrs2/store.py@7536139:166-170
    [[nodiscard]] std::vector<std::string> proposition_ids(
        std::string_view proposition) const override;
    // SWEGCA: src/swegca_vrs2/store.py@7536139:149-153
    [[nodiscard]] std::optional<std::string> successor_of(
        std::string_view identifier) const override;
    // SWEGCA: src/swegca_vrs2/store.py@7536139:172-174
    [[nodiscard]] std::string snapshot_id() const override;
    // SWEGCA: src/swegca_vrs2/store.py@7536139:174-174
    [[nodiscard]] std::uint64_t outcome_count(
        std::string_view outcome) const override;
    // SWEGCA: src/swegca_vrs2/store.py@7536139:129-135
    [[nodiscard]] std::vector<std::string> episode_ids_for_cue(
        std::string_view cue) const override;
    // SWEGCA: src/swegca_vrs2/engine/mosaic_semantic_family_directory.py@7536139:12-19
    [[nodiscard]] std::vector<std::string> semantic_family_parents(
        std::string_view identifier) const override;
    // SWEGCA: src/swegca_vrs2/engine/mosaic_semantic_family_directory.py@7536139:22-27
    [[nodiscard]] std::vector<std::string> semantic_family_members(
        std::string_view parent) const override;
    // SWEGCA: src/swegca_vrs2/engine/mosaic_memory_activation.py@7536139:254-259
    [[nodiscard]] std::uint64_t posting_count(std::string_view cue) const override;
    // SWEGCA: src/swegca_vrs2/engine/mosaic_memory_activation.py@7536139:257-259
    [[nodiscard]] std::uint64_t exact_union_count(
        std::span<const std::string> cues) const override;

    // SWEGCA: src/swegca_vrs2/store.py@7536139:371-405
    [[nodiscard]] const std::string& pair_snapshot_id() const {
        return pair_snapshot_id_;
    }
    // SWEGCA: src/swegca_vrs2/store.py@7536139:371-405
    [[nodiscard]] std::int64_t published_row_limit() const {
        return published_row_limit_;
    }
    // SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:691-713
    [[nodiscard]] const ExactJournalDirectory& originals() const {
        return *originals_;
    }
    // SWEGCA: src/swegca_vrs2/cue_shards.py@c06092a:392-440
    [[nodiscard]] const NativeCueDirectory& cues() const { return *cues_; }
    // SWEGCA: src/swegca_vrs2/store.py@7536139:166-170
    [[nodiscard]] const NativeCueDirectory& propositions() const {
        return *propositions_;
    }
    // SWEGCA: src/swegca_vrs2/store.py@7536139:149-175
    [[nodiscard]] const NativeCueDirectory& successors() const {
        return *successors_;
    }
    // SWEGCA: src/swegca_vrs2/store.py@c06092a:1299-1303
    [[nodiscard]] const NativeCueDirectory& sources() const {
        return *sources_;
    }

private:
    HotIndexSeed memory_;
    std::string pair_snapshot_id_;
    std::int64_t published_row_limit_;
    std::shared_ptr<const ExactJournalDirectory> originals_;
    std::shared_ptr<const HotIndexProjectionLog> headers_;
    std::shared_ptr<const NativeCueDirectory> cues_;
    std::shared_ptr<const NativeCueDirectory> propositions_;
    std::shared_ptr<const NativeCueDirectory> successors_;
    std::shared_ptr<const NativeCueDirectory> sources_;
    std::vector<std::shared_ptr<const RecordedSemanticFamilyRead>>
        semantic_families_;
};

}  // namespace swegca::vrs
