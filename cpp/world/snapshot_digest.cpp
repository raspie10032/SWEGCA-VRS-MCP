#include "world/snapshot_digest.hpp"

#include "swegca_architecture/sha256.hpp"
#include "world/memory_activation.hpp"
#include "world/semantic_vrs_ingress.hpp"

namespace swegca::world {
namespace {

std::string hex(const architecture::DigestBytes& digest) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result(digest.size() * 2, '0');
    for (std::size_t i = 0; i != digest.size(); ++i) {
        const auto value = std::to_integer<unsigned>(digest[i]);
        result[i * 2] = digits[value >> 4U];
        result[i * 2 + 1] = digits[value & 15U];
    }
    return result;
}

}  // namespace

std::string snapshot_digest(
    const std::map<std::string, MemoryEpisode, std::less<>>& episodes,
    const std::map<std::string, std::vector<std::string>, std::less<>>& postings) {
    JsonValue::Array episode_rows;
    for (const auto& [unused, episode] : episodes) {
        (void)unused;
        JsonValue::Array steps;
        for (const auto& step : episode.steps) {
            JsonValue::Array relations;
            for (const auto& value : step.relations) relations.emplace_back(value);
            JsonValue::Array evidence;
            for (const auto& value : step.evidence_refs) evidence.emplace_back(value);
            steps.emplace_back(JsonValue::Object{
                {"evidence_refs", JsonValue(std::move(evidence))},
                {"judgment", step.judgment}, {"observation", step.observation},
                {"outcome", step.outcome}, {"phase", step.phase},
                {"relations", JsonValue(std::move(relations))}});
        }
        JsonValue::Array cues;
        for (const auto& value : episode.cues) cues.emplace_back(value);
        JsonValue::Array addresses;
        for (const auto& value : episode.source_addresses) addresses.emplace_back(value);
        episode_rows.emplace_back(JsonValue::Object{
            {"cues", JsonValue(std::move(cues))}, {"episode_id", episode.episode_id},
            {"revision", episode.revision}, {"source_addresses", JsonValue(std::move(addresses))},
            {"steps", JsonValue(std::move(steps))},
            {"verification_state", episode.verification_state}});
    }
    JsonValue::Object posting_rows;
    for (const auto& [cue, identifiers] : postings) {
        JsonValue::Array values;
        for (const auto& identifier : identifiers) values.emplace_back(identifier);
        posting_rows.emplace(cue, JsonValue(std::move(values)));
    }
    const auto wire = semantic_canonical_json(JsonValue::Object{
        {"episodes", JsonValue(std::move(episode_rows))},
        {"postings", JsonValue(std::move(posting_rows))}});
    architecture::Sha256 hash;
    hash.update(wire);
    return hex(hash.finish());
}

}  // namespace swegca::world
