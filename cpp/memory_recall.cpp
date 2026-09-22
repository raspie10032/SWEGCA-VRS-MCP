#include "memory_recall.hpp"

#include "keys.hpp"
#include "unicode.hpp"

#include <algorithm>
#include <set>
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

// SWEGCA: src/swegca_vrs2/engine/mosaic_memory_activation.py@7536139:280-298
RecallResult::RecallResult(std::string new_query, std::vector<RecallCandidate> new_candidates,
    std::string new_snapshot_id, std::vector<std::pair<std::string, std::string>> new_source_dependencies,
    bool new_codex_per_item_allowlist_used, bool new_action_authorized, bool new_persistent_write_authorized)
    : query(std::move(new_query)), candidates(std::move(new_candidates)),
      snapshot_id(std::move(new_snapshot_id)), codex_per_item_allowlist_used(new_codex_per_item_allowlist_used),
      action_authorized(new_action_authorized), persistent_write_authorized(new_persistent_write_authorized),
      source_dependencies(std::move(new_source_dependencies)) {
    if (!nonblank(query)) throw std::runtime_error("recall query must not be empty");
    if (!nonblank(snapshot_id)) throw std::runtime_error("snapshot_id must not be empty");
    if (codex_per_item_allowlist_used || action_authorized || persistent_write_authorized)
        throw std::runtime_error("recall authority changed");
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_memory_activation.py@7536139:301-348
RecallResult recall_memory(const PublishedHotIndex& index, const DejaVuSignal& signal,
                           const std::vector<std::string>& navigation_cues) {
    if (signal.snapshot_id != index.snapshot_id())
        throw std::runtime_error("déjà vu snapshot changed before recall");

    std::vector<std::string> navigation;
    std::unordered_set<std::string> navigation_seen;
    for (const auto& cue : navigation_cues) {
        auto normalized = normalize_cue(cue);
        if (navigation_seen.insert(normalized).second)
            navigation.push_back(std::move(normalized));
    }

    std::vector<std::string> selected_cues = signal.matched_cues;
    std::unordered_set<std::string> selected_seen(selected_cues.begin(), selected_cues.end());
    for (const auto& cue : navigation)
        if (selected_seen.insert(cue).second) selected_cues.push_back(cue);

    std::set<std::string> identifiers;
    for (const auto& cue : selected_cues)
        for (const auto& identifier : index.episode_ids_for_cue(cue))
            identifiers.insert(identifier);

    std::set<std::string> families;
    for (const auto& identifier : identifiers)
        for (const auto& parent : index.semantic_family_parents(identifier))
            families.insert(parent);
    std::set<std::pair<std::string, std::string>> dependencies;
    for (const auto& parent : families) {
        identifiers.insert(parent);
        dependencies.emplace(parent, parent);
        for (const auto& member : index.semantic_family_members(parent)) {
            identifiers.insert(member);
            dependencies.emplace(member, parent);
        }
    }

    std::unordered_set<std::string> current(signal.current_cues.begin(), signal.current_cues.end());
    current.insert(navigation.begin(), navigation.end());
    std::vector<RecallCandidate> candidates;
    candidates.reserve(identifiers.size());
    for (const auto& identifier : identifiers) {
        const auto header = index.episode_header(identifier);
        std::vector<std::string> matched;
        std::unordered_set<std::string> union_cues = current;
        for (const auto& cue : header.cues) {
            if (current.contains(cue)) matched.push_back(cue);
            union_cues.insert(cue);
        }
        const auto overlap = static_cast<double>(matched.size()) /
                             static_cast<double>(union_cues.size());
        candidates.push_back(RecallCandidate{identifier, std::move(matched), overlap,
            header.revision, header.verification_state, header.historical_outcomes});
    }
    std::sort(candidates.begin(), candidates.end(), [](const auto& left, const auto& right) {
        if (left.cue_overlap != right.cue_overlap) return left.cue_overlap > right.cue_overlap;
        return left.episode_id < right.episode_id;
    });
    return RecallResult(signal.query, std::move(candidates), index.snapshot_id(),
                        std::vector<std::pair<std::string, std::string>>(
                            dependencies.begin(), dependencies.end()));
}

// SWEGCA: src/swegca_vrs2/store.py@c06092a:1719-1723
RecallResult select_replay_original(const PublishedHotIndex& index,
                                    const RecallResult& complete) {
    if (complete.snapshot_id != index.snapshot_id())
        throw std::runtime_error("recall snapshot changed before replay");
    const RecallCandidate* selected = nullptr;
    for (const auto& candidate : complete.candidates) {
        if (!index.successor_of(candidate.episode_id)) {
            selected = &candidate;
            break;
        }
    }
    if (!selected && !complete.candidates.empty()) selected = &complete.candidates.front();
    std::vector<RecallCandidate> opened;
    if (selected) opened.push_back(*selected);
    return RecallResult(complete.query, std::move(opened), complete.snapshot_id,
                        complete.source_dependencies);
}

}  // namespace swegca::vrs
