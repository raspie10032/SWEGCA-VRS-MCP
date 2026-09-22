#include "memory_episode.hpp"

#include "digest.hpp"
#include "keys.hpp"
#include "unicode.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>
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

// SWEGCA: src/swegca_vrs2/engine/mosaic_memory_activation.py@7536139:9
bool valid_outcome(std::string_view outcome) {
    constexpr std::array<std::string_view, 6> outcomes{
        "success", "failure", "negative", "uncertain", "conflict", "pending"};
    return std::find(outcomes.begin(), outcomes.end(), outcome) != outcomes.end();
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_memory_activation.py@7536139:79-81
std::vector<std::string> unique_cues(std::vector<std::string> cues) {
    std::vector<std::string> result;
    std::unordered_set<std::string> seen;
    result.reserve(cues.size());
    for (const auto& cue : cues) {
        auto normalized = normalize_cue(cue);
        if (seen.insert(normalized).second) result.push_back(std::move(normalized));
    }
    return result;
}

}  // namespace

// SWEGCA: src/swegca_vrs2/engine/mosaic_memory_activation.py@7536139:44-67
MemoryStep::MemoryStep(std::string new_phase, Json new_observation,
                       std::vector<std::string> new_relations,
                       std::string new_judgment, std::string new_outcome,
                       std::vector<std::string> new_evidence_refs)
    : phase(std::move(new_phase)), observation(std::move(new_observation)),
      relations(std::move(new_relations)), judgment(std::move(new_judgment)),
      outcome(std::move(new_outcome)), evidence_refs(std::move(new_evidence_refs)) {
    if (!nonblank(phase)) throw std::runtime_error("memory phase must not be empty");
    if (!nonblank(judgment)) throw std::runtime_error("memory judgment must not be empty");
    if (!valid_outcome(outcome)) throw std::runtime_error("unsupported historical outcome");
    if (evidence_refs.empty() ||
        std::any_of(evidence_refs.begin(), evidence_refs.end(), [](const auto& ref) {
            return !nonblank(ref);
        }))
        throw std::runtime_error("memory step requires evidence provenance");
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_memory_activation.py@7536139:70-90
MemoryEpisode::MemoryEpisode(std::string new_episode_id,
                             std::vector<std::string> new_cues, std::vector<MemoryStep> new_steps,
                             std::vector<std::string> new_source_addresses, std::string new_revision,
                             std::string new_verification_state)
    : episode_id(std::move(new_episode_id)), cues(unique_cues(std::move(new_cues))),
      steps(std::move(new_steps)), source_addresses(std::move(new_source_addresses)),
      revision(std::move(new_revision)),
      verification_state(std::move(new_verification_state)) {
    if (!nonblank(episode_id)) throw std::runtime_error("episode_id must not be empty");
    if (cues.empty() || steps.empty() || source_addresses.empty())
        throw std::runtime_error("memory episode is incomplete");
    if (std::unordered_set<std::string>(source_addresses.begin(), source_addresses.end()).size()
        != source_addresses.size())
        throw std::runtime_error("memory source addresses must be unique");
    if (!nonblank(revision)) throw std::runtime_error("memory revision must not be empty");
    if (!nonblank(verification_state))
        throw std::runtime_error("verification state must not be empty");
}

// SWEGCA: src/swegca_vrs2/store.py@7536139:154-158
std::vector<std::string> postings_cues_from_observation(const Json& row) {
    auto cues = lexical_keys(row.at("text").string());
    std::unordered_set<std::string> seen(cues.begin(), cues.end());
    for (const auto& extra : row.at("cues").array()) {
        auto folded = casefold_text(extra.string());
        if (seen.insert(folded).second) cues.push_back(std::move(folded));
    }
    const auto& proposition = row.at("proposition");
    if (!std::holds_alternative<std::nullptr_t>(proposition.data))
        cues.push_back("proposition:" + proposition.string());
    if (cues.empty()) cues.push_back("source:" + row.at("source").string());
    return cues;
}

// SWEGCA: src/swegca_vrs2/store.py@7536139:145-166
MemoryEpisode episode_from_observation(const Json& row) {
    auto identity = row.object();
    identity.erase("request_id");
    const auto identifier = "memory:" + sha256_hex(Json(std::move(identity)).canonical());
    auto cues = postings_cues_from_observation(row);
    const auto& proposition = row.at("proposition");
    Json::Object evidence;
    evidence.emplace("text", row.at("text"));
    evidence.emplace("metadata", row.at("metadata"));
    evidence.emplace("proposition_id", proposition);
    evidence.emplace("evidence_polarity", row.at("polarity"));
    evidence.emplace("supersedes", row.at("supersedes"));
    evidence.emplace("current_truth_claimed", Json(false));
    const auto source = row.at("source").string();
    std::vector<MemoryStep> steps;
    steps.emplace_back("external_observation", Json(std::move(evidence)),
                       std::vector<std::string>{},
                       "Recorded external observation; truth and action authority not granted.",
                       row.at("outcome").string(), std::vector<std::string>{source});
    return MemoryEpisode(identifier, std::move(cues), std::move(steps),
                         {source}, row.at("revision").string(), "unverified");
}

}  // namespace swegca::vrs
