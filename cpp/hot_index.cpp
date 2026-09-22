#include "hot_index.hpp"

#include "digest.hpp"

#include <array>
#include <stdexcept>
#include <utility>

namespace swegca::vrs {
namespace {

// SWEGCA: src/swegca_vrs2/store.py@7536139:146-146
std::string observation_identifier(const Json& row) {
    auto body = row.object();
    body.erase("request_id");
    return "memory:" + sha256_hex(Json(std::move(body)).canonical());
}

// SWEGCA: src/swegca_vrs2/store.py@7536139:172-172
std::string next_snapshot_id(std::string_view previous, std::string_view identifier) {
    Json::Array values;
    values.emplace_back(std::string(previous));
    values.emplace_back(std::string(identifier));
    return sha256_hex(Json(std::move(values)).canonical());
}

}  // namespace

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
    const auto identifier = observation_identifier(row);
    if (index.contains_episode(identifier))
        return HotIndexAppendPlan{identifier};

    std::optional<std::string> previous;
    if (!std::holds_alternative<std::nullptr_t>(row.at("supersedes").data)) {
        previous = row.at("supersedes").string();
        const auto old = index.prior_episode(*previous);
        if (old.source_addresses != std::vector<std::string>{row.at("source").string()} ||
            old.revision == row.at("revision").string() ||
            index.contains_superseded(*previous))
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
        std::move(previous), next_snapshot_id(index.snapshot_id(), identifier),
        index.outcome_count(row.at("outcome").string()) + 1};
}

}  // namespace swegca::vrs
