#include "hot_index_pending.hpp"

#include <stdexcept>
#include <utility>

namespace swegca::vrs {
namespace {

// SWEGCA: src/swegca_vrs2/store.py@7536139:216-240
HotIndexEpisodeHeader header_of(const MemoryEpisode& episode) {
    const auto& observation = episode.steps.at(0).observation;
    const auto optional_text = [&](std::string_view key) -> std::optional<std::string> {
        const auto& value = observation.at(key);
        if (std::holds_alternative<std::nullptr_t>(value.data)) return std::nullopt;
        return value.string();
    };
    std::vector<std::string> outcomes;
    outcomes.reserve(episode.steps.size());
    for (const auto& step : episode.steps) outcomes.push_back(step.outcome);
    return HotIndexEpisodeHeader{
        episode.episode_id, episode.cues, episode.source_addresses,
        episode.revision, episode.verification_state, std::move(outcomes),
        optional_text("proposition_id"), optional_text("evidence_polarity"),
        optional_text("supersedes")};
}

}  // namespace

// SWEGCA: src/swegca_vrs2/store.py@7536139:342-348
HotIndexPending::HotIndexPending(const HotIndexRead& published)
    : published_(published), snapshot_id_(published.snapshot_id()) {}

// SWEGCA: src/swegca_vrs2/store.py@7536139:391-400
void HotIndexPending::ensure_valid() const {
    if (failed_) throw std::runtime_error("pending hot index must be discarded after failure");
}

// SWEGCA: src/swegca_vrs2/store.py@7536139:342-348
std::pair<std::string, bool> HotIndexPending::append(const Json& row) {
    ensure_valid();
    try {
        auto plan = plan_hot_index_append(*this, row);
        auto identifier = plan.identifier;
        if (!plan.added()) return {std::move(identifier), false};
        const auto position = plans_.size();
        snapshot_id_ = *plan.new_snapshot_id;
        pending_outcomes_[*plan.outcome] = *plan.new_outcome_count;
        if (plan.supersedes) pending_successors_[*plan.supersedes] = identifier;
        if (plan.proposition) pending_propositions_[*plan.proposition].push_back(identifier);
        plans_.push_back(std::move(plan));
        pending_ids_.emplace(identifier, position);
        return {std::move(identifier), true};
    } catch (...) {
        failed_ = true;
        throw;
    }
}

// SWEGCA: src/swegca_vrs2/store.py@7536139:146-148
bool HotIndexPending::contains_episode(std::string_view identifier) const {
    ensure_valid();
    return pending_ids_.contains(std::string(identifier)) || published_.contains_episode(identifier);
}

// SWEGCA: src/swegca_vrs2/store.py@7536139:216-240
HotIndexEpisodeHeader HotIndexPending::episode_header(std::string_view identifier) const {
    ensure_valid();
    const auto found = pending_ids_.find(std::string(identifier));
    if (found != pending_ids_.end()) return header_of(*plans_[found->second].episode);
    return published_.episode_header(identifier);
}

// SWEGCA: src/swegca_vrs2/store.py@7536139:222-236
std::vector<std::string> HotIndexPending::proposition_ids(std::string_view proposition) const {
    ensure_valid();
    auto ids = published_.proposition_ids(proposition);
    const auto found = pending_propositions_.find(std::string(proposition));
    if (found != pending_propositions_.end())
        ids.insert(ids.end(), found->second.begin(), found->second.end());
    return ids;
}

// SWEGCA: src/swegca_vrs2/store.py@7536139:149-153
std::optional<std::string> HotIndexPending::successor_of(std::string_view identifier) const {
    ensure_valid();
    const auto found = pending_successors_.find(std::string(identifier));
    if (found != pending_successors_.end()) return found->second;
    return published_.successor_of(identifier);
}

// SWEGCA: src/swegca_vrs2/store.py@7536139:172-174
std::string HotIndexPending::snapshot_id() const {
    ensure_valid();
    return snapshot_id_;
}

// SWEGCA: src/swegca_vrs2/store.py@7536139:174-174
std::uint64_t HotIndexPending::outcome_count(std::string_view outcome) const {
    ensure_valid();
    const auto found = pending_outcomes_.find(std::string(outcome));
    if (found != pending_outcomes_.end()) return found->second;
    return published_.outcome_count(outcome);
}

}  // namespace swegca::vrs
