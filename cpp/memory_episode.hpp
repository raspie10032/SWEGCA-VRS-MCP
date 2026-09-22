#pragma once

#include "json.hpp"

#include <string>
#include <vector>

namespace swegca::vrs {

struct MemoryStep {
    std::string phase;
    Json observation;
    std::vector<std::string> relations;
    std::string judgment;
    std::string outcome;
    std::vector<std::string> evidence_refs;

    // SWEGCA: src/swegca_vrs2/engine/mosaic_memory_activation.py@7536139:44-67
    MemoryStep(std::string phase, Json observation,
               std::vector<std::string> relations, std::string judgment,
               std::string outcome, std::vector<std::string> evidence_refs);
};

struct MemoryEpisode {
    std::string episode_id;
    std::vector<std::string> cues;
    std::vector<MemoryStep> steps;
    std::vector<std::string> source_addresses;
    std::string revision;
    std::string verification_state;

    // SWEGCA: src/swegca_vrs2/engine/mosaic_memory_activation.py@7536139:70-90
    MemoryEpisode(std::string episode_id, std::vector<std::string> cues,
                  std::vector<MemoryStep> steps,
                  std::vector<std::string> source_addresses,
                  std::string revision, std::string verification_state);
};

// Build the author's exact original episode from one normalized observation.
// SWEGCA: src/swegca_vrs2/store.py@7536139:145-166
[[nodiscard]] MemoryEpisode episode_from_observation(const Json& row);

// The request ID does not enter the original episode identifier.
// SWEGCA: src/swegca_vrs2/store.py@7536139:145-148
[[nodiscard]] std::string episode_id_from_observation(const Json& row);

// Keep the author's pre-episode posting keys distinct from episode.cues.
// SWEGCA: src/swegca_vrs2/store.py@7536139:154-158
[[nodiscard]] std::vector<std::string> postings_cues_from_observation(const Json& row);

}  // namespace swegca::vrs
