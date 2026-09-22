#pragma once

#include "hot_index.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace swegca::vrs {

class DejaVuSignal {
public:
    const std::string snapshot_id;
    const std::string query;
    const std::vector<std::string> current_cues;
    const std::vector<std::string> matched_cues;
    const double recognition_strength;
    const std::int64_t candidate_count;
    const bool memory_identifiers_exposed;
    const bool action_authorized;

    // SWEGCA: src/swegca_vrs2/engine/mosaic_memory_activation.py@7536139:161-178
    DejaVuSignal(std::string snapshot_id, std::string query,
                 std::vector<std::string> current_cues,
                 std::vector<std::string> matched_cues, double recognition_strength,
                 std::int64_t candidate_count, bool memory_identifiers_exposed = false,
                 bool action_authorized = false);

    // SWEGCA: src/swegca_vrs2/engine/mosaic_memory_activation.py@7536139:180-182
    [[nodiscard]] bool triggered() const { return candidate_count > 0; }
};

// The caller invokes this with the host input before any other memory-store
// operation. It exposes no episode identifier or original content.
// SWEGCA: src/swegca_vrs2/engine/mosaic_memory_activation.py@7536139:246-267
[[nodiscard]] DejaVuSignal detect_deja_vu(const PublishedHotIndex& index,
                                          std::string query,
                                          const std::vector<std::string>& current_cues);

}  // namespace swegca::vrs
