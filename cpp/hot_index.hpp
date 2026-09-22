#pragma once

#include "memory_episode.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace swegca::vrs {

// Main reads these fields for supersedes and explicit-proposition Graph updates
// before Replay opens the original body.
struct HotIndexEpisodeHeader {
    std::string episode_id;
    std::vector<std::string> cues;
    std::vector<std::string> source_addresses;
    std::string revision;
    std::string verification_state;
    std::vector<std::string> historical_outcomes;
    std::optional<std::string> proposition_id;
    std::optional<std::string> evidence_polarity;
    std::optional<std::string> supersedes;
};

// The unpublished view and the physical projection must derive identical
// header fields from the author's one original episode.
// SWEGCA: src/swegca_vrs2/store.py@7536139:158-175
[[nodiscard]] HotIndexEpisodeHeader hot_index_header_from_episode(
    const MemoryEpisode& episode);

// The derived physical index supplies these reads. This surface does not
// replace the main owner or create a second memory decision system.
class HotIndexRead {
public:
    virtual ~HotIndexRead() = default;
    // SWEGCA: src/swegca_vrs2/store.py@7536139:146-153
    [[nodiscard]] virtual bool contains_episode(std::string_view identifier) const = 0;
    // SWEGCA: src/swegca_vrs2/store.py@7536139:149-153
    [[nodiscard]] virtual HotIndexEpisodeHeader episode_header(std::string_view identifier) const = 0;
    // SWEGCA: src/swegca_vrs2/store.py@7536139:222-236
    [[nodiscard]] virtual std::vector<std::string> proposition_ids(std::string_view proposition) const = 0;
    // SWEGCA: src/swegca_vrs2/store.py@7536139:149-153
    [[nodiscard]] virtual std::optional<std::string> successor_of(std::string_view identifier) const = 0;
    // SWEGCA: src/swegca_vrs2/store.py@7536139:172-174
    [[nodiscard]] virtual std::string snapshot_id() const = 0;
    // SWEGCA: src/swegca_vrs2/store.py@7536139:174-174
    [[nodiscard]] virtual std::uint64_t outcome_count(std::string_view outcome) const = 0;
};

// Only a committed immutable physical generation can be published in the
// full-current pair. HotIndexPending is deliberately not a published index.
class PublishedHotIndex : public HotIndexRead {
public:
    ~PublishedHotIndex() override = default;
    // SWEGCA: src/swegca_vrs2/engine/mosaic_memory_activation.py@7536139:317-320
    [[nodiscard]] virtual std::vector<std::string> episode_ids_for_cue(
        std::string_view cue) const = 0;
    // A recorded parent is returned for an ID that is itself a parent, and
    // all recorded parents are returned for an ID that is a child.
    // SWEGCA: src/swegca_vrs2/engine/mosaic_semantic_family_directory.py@7536139:12-19
    [[nodiscard]] virtual std::vector<std::string> semantic_family_parents(
        std::string_view identifier) const = 0;
    // Return the complete recorded child span of this parent; Recall adds the
    // parent itself as the first span.
    // SWEGCA: src/swegca_vrs2/engine/mosaic_semantic_family_directory.py@7536139:22-27
    [[nodiscard]] virtual std::vector<std::string> semantic_family_members(
        std::string_view parent) const = 0;
    // SWEGCA: src/swegca_vrs2/engine/mosaic_memory_activation.py@7536139:254-259
    [[nodiscard]] virtual std::uint64_t posting_count(std::string_view cue) const = 0;
    // Exact distinct cardinality of the complete posting union. It is a
    // physical representation of the author's set union, not a sample.
    // SWEGCA: src/swegca_vrs2/engine/mosaic_memory_activation.py@7536139:257-259
    [[nodiscard]] virtual std::uint64_t exact_union_count(
        std::span<const std::string> cues) const = 0;
};

struct HotIndexSeed {
    std::string snapshot_id;
    std::map<std::string, std::uint64_t> outcome_counts;
};

// SWEGCA: src/swegca_vrs2/store.py@7536139:336-336
[[nodiscard]] HotIndexSeed empty_hot_index(std::string_view identity);

// A duplicate carries only its existing identifier. An addition carries the
// entire author's append delta for the physical index to publish atomically.
struct HotIndexAppendPlan {
    std::string identifier;
    std::optional<MemoryEpisode> episode;
    std::vector<std::string> posting_cues;
    std::optional<std::string> proposition;
    std::optional<std::string> supersedes;
    std::optional<std::string> outcome;
    std::optional<std::string> new_snapshot_id;
    std::optional<std::uint64_t> new_outcome_count;

    // SWEGCA: src/swegca_vrs2/store.py@7536139:147-148
    [[nodiscard]] bool added() const { return episode.has_value(); }
};

// Input row has already passed observation(). The plan is not published until
// the main owner commits the journal and the derived index generation.
// SWEGCA: src/swegca_vrs2/store.py@7536139:145-175
[[nodiscard]] HotIndexAppendPlan plan_hot_index_append(const HotIndexRead& index,
                                                       const Json& row);

}  // namespace swegca::vrs
