#include "deja_vu.hpp"

#include "keys.hpp"
#include "unicode.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace swegca::vrs {
namespace {

// SWEGCA: src/swegca_vrs2/engine/mosaic_memory_activation.py@7536139:15-19
bool nonblank(std::string_view value) {
    const auto points = decode_utf8(value);
    return std::any_of(points.begin(), points.end(), [](auto point) {
        return !python_space(point);
    });
}

}  // namespace

// SWEGCA: src/swegca_vrs2/engine/mosaic_memory_activation.py@7536139:161-178
DejaVuSignal::DejaVuSignal(std::string new_snapshot_id, std::string new_query,
                           std::vector<std::string> new_current_cues, std::vector<std::string> new_matched_cues,
                           double new_recognition_strength, std::int64_t new_candidate_count,
                           bool new_memory_identifiers_exposed, bool new_action_authorized)
    : snapshot_id(std::move(new_snapshot_id)), query(std::move(new_query)),
      current_cues(std::move(new_current_cues)), matched_cues(std::move(new_matched_cues)),
      recognition_strength(new_recognition_strength), candidate_count(new_candidate_count),
      memory_identifiers_exposed(new_memory_identifiers_exposed), action_authorized(new_action_authorized) {
    if (!nonblank(snapshot_id)) throw std::runtime_error("snapshot_id must not be empty");
    if (!nonblank(query)) throw std::runtime_error("déjà vu query must not be empty");
    if (!(recognition_strength >= 0.0 && recognition_strength <= 1.0) || candidate_count < 0)
        throw std::runtime_error("déjà vu signal metrics changed");
    if (memory_identifiers_exposed || action_authorized)
        throw std::runtime_error("déjà vu is only an anonymous retrieval trigger");
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_memory_activation.py@7536139:246-267
DejaVuSignal detect_deja_vu(const PublishedHotIndex& index, std::string query,
                            const std::vector<std::string>& current_cues) {
    std::vector<std::string> cues;
    cues.reserve(current_cues.size());
    std::unordered_set<std::string> seen;
    for (const auto& cue : current_cues) {
        auto normalized = normalize_cue(cue);
        if (seen.insert(normalized).second) cues.push_back(std::move(normalized));
    }
    std::vector<std::string> matched;
    for (const auto& cue : cues)
        if (index.posting_count(cue) != 0) matched.push_back(cue);
    const auto count = matched.empty() ? 0 : index.exact_union_count(matched);
    if (count > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        throw std::runtime_error("déjà vu signal metrics changed");
    const auto strength = static_cast<double>(matched.size()) /
                          static_cast<double>(std::max<std::size_t>(1, cues.size()));
    return DejaVuSignal(index.snapshot_id(), std::move(query), std::move(cues),
                        std::move(matched), strength, static_cast<std::int64_t>(count));
}

}  // namespace swegca::vrs
