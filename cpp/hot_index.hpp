#pragma once

#include "memory_episode.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace swegca::vrs {

// Only the fields read by the author's supersedes check are needed before
// Replay opens the original episode body.
struct HotIndexPriorEpisode {
    std::vector<std::string> source_addresses;
    std::string revision;
};

// The derived physical index supplies these reads. This surface does not
// replace the main owner or create a second memory decision system.
class HotIndexRead {
public:
    virtual ~HotIndexRead() = default;
    // SWEGCA: src/swegca_vrs2/store.py@7536139:146-153
    [[nodiscard]] virtual bool contains_episode(std::string_view identifier) const = 0;
    // SWEGCA: src/swegca_vrs2/store.py@7536139:149-153
    [[nodiscard]] virtual HotIndexPriorEpisode prior_episode(std::string_view identifier) const = 0;
    // SWEGCA: src/swegca_vrs2/store.py@7536139:149-153
    [[nodiscard]] virtual bool contains_superseded(std::string_view identifier) const = 0;
    // SWEGCA: src/swegca_vrs2/store.py@7536139:172-174
    [[nodiscard]] virtual std::string snapshot_id() const = 0;
    // SWEGCA: src/swegca_vrs2/store.py@7536139:174-174
    [[nodiscard]] virtual std::uint64_t outcome_count(std::string_view outcome) const = 0;
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
