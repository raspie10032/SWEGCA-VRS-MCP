#pragma once

#include "hot_index.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace swegca::vrs {

// A derived metadata row, bound to one committed original journal sequence.
// It contains no original observation body and cannot become a second source
// of evidence. Raw posting keys remain distinct from normalized episode cues.
// SWEGCA: src/swegca_vrs2/store.py@7536139:145-175
struct HotIndexProjectionRow {
    std::int64_t journal_sequence;
    std::string pair_snapshot_id;
    HotIndexEpisodeHeader header;
    std::vector<std::string> posting_cues;
};

// SWEGCA: src/swegca_vrs2/store.py@7536139:145-175
[[nodiscard]] HotIndexProjectionRow project_hot_index_addition(
    const HotIndexAppendPlan& plan, std::int64_t journal_sequence,
    std::string pair_snapshot_id);

// A checksummed, length-delimited native projection frame. It is rebuildable
// from the original journal and never replaces that journal as evidence.
// SWEGCA: src/swegca_vrs2/store.py@7536139:145-175
[[nodiscard]] std::vector<std::byte> encode_hot_index_projection(
    const HotIndexProjectionRow& row);

// SWEGCA: src/swegca_vrs2/store.py@7536139:145-175
[[nodiscard]] HotIndexProjectionRow decode_hot_index_projection(
    std::span<const std::byte> frame);

}  // namespace swegca::vrs
