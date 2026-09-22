#include "hot_index.hpp"

#include "digest.hpp"

#include <array>
#include <stdexcept>
#include <utility>

namespace swegca::vrs {
// SWEGCA: src/swegca_vrs2/store.py@7536139:172-172
std::string next_hot_index_snapshot_id(std::string_view previous,
                                       std::string_view identifier) {
    Json::Array values;
    values.emplace_back(std::string(previous));
    values.emplace_back(std::string(identifier));
    return sha256_hex(Json(std::move(values)).canonical());
}

// SWEGCA: src/swegca_vrs2/store.py@7536139:158-175
HotIndexEpisodeHeader hot_index_header_from_episode(const MemoryEpisode& episode) {
    const auto& observed = episode.steps.at(0).observation;
    const auto optional_text = [&](std::string_view key) -> std::optional<std::string> {
        const auto& value = observed.at(key);
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

// SWEGCA: src/swegca_vrs2/store.py@7536139:336-336
HotIndexSeed empty_hot_index(std::string_view identity) {
    Json::Array values;
    values.emplace_back(std::string("memory"));
    values.emplace_back(std::string(identity));
    HotIndexSeed seed;
    seed.snapshot_id = sha256_hex(Json(std::move(values)).canonical());
    constexpr std::array<std::string_view, 6> outcomes{
        "success", "failure", "negative", "uncertain", "conflict", "pending"};
    for (const auto outcome : outcomes) seed.outcome_counts.emplace(outcome, 0);
    return seed;
}

// SWEGCA: src/swegca_vrs2/store.py@7536139:145-175
HotIndexAppendPlan plan_hot_index_append(const HotIndexRead& index, const Json& row) {
    const auto identifier = episode_id_from_observation(row);
    if (index.contains_episode(identifier))
        return HotIndexAppendPlan{identifier};

    std::optional<std::string> previous;
    if (!std::holds_alternative<std::nullptr_t>(row.at("supersedes").data)) {
        previous = row.at("supersedes").string();
        const auto old = index.episode_header(*previous);
        if (old.source_addresses != std::vector<std::string>{row.at("source").string()} ||
            old.revision == row.at("revision").string() ||
            index.successor_of(*previous).has_value())
            throw std::runtime_error("invalid_source_revision_successor");
    }

    auto posting_cues = postings_cues_from_observation(row);
    auto episode = episode_from_observation(row);
    std::optional<std::string> proposition;
    const auto& value = row.at("proposition");
    if (!std::holds_alternative<std::nullptr_t>(value.data) && !value.string().empty())
        proposition = value.string();

    return HotIndexAppendPlan{
        identifier, std::move(episode), std::move(posting_cues), std::move(proposition),
        std::move(previous), row.at("outcome").string(),
        next_hot_index_snapshot_id(index.snapshot_id(), identifier),
        index.outcome_count(row.at("outcome").string()) + 1};
}

}  // namespace swegca::vrs
