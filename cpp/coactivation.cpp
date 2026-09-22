#include "coactivation.hpp"

#include "unicode.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace swegca::vrs {
namespace {

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_coactivation.py@c06092a:110-113
bool nonblank(std::string_view value) {
    const auto points = decode_utf8(value);
    return std::any_of(points.begin(), points.end(), [](std::uint32_t point) {
        return !python_space(point);
    });
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_coactivation.py@c06092a:43-68
bool same_original_step(const MemoryStep& left, const MemoryStep& right) {
    return left.phase == right.phase &&
        left.relations == right.relations &&
        left.judgment == right.judgment &&
        left.outcome == right.outcome &&
        left.evidence_refs == right.evidence_refs &&
        left.observation.canonical() == right.observation.canonical();
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_coactivation.py@c06092a:43-68
bool same_original_steps(const std::vector<MemoryStep>& left,
                         const std::vector<MemoryStep>& right) {
    if (left.size() != right.size()) return false;
    for (std::size_t at = 0; at < left.size(); ++at)
        if (!same_original_step(left[at], right[at])) return false;
    return true;
}

}  // namespace

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_coactivation.py@c06092a:107-158
CoactivationEvent prepare_coactivation_event(
    const FullCurrentMemoryVrsSnapshot& pair,
    const MemoryActivationReceipt& activation, std::string request_id,
    std::int64_t observed_at_ns, const EventVrsInputView& inputs,
    const GraphNodeDirectory& nodes, const GraphRegionDirectory& regions) {
    if (!nonblank(request_id) || observed_at_ns < 0)
        throw std::runtime_error("main-issued request identity and nonnegative time required");
    validate_opened_identity(activation);
    if (activation.snapshot_id != pair.memory().snapshot_id() ||
        pair.vrs_snapshot_id() != inputs.snapshot_id())
        throw std::runtime_error("activation does not belong to pinned memory");
    nodes.require_source(inputs);
    regions.require_source(inputs);
    regions.require_memory_source(pair.memory());
    std::set<std::string> identifiers;
    std::vector<CoactivatedExperience> rows;
    rows.reserve(activation.recall.candidates.size());
    std::set<std::string> topologies;
    for (std::size_t at = 0; at < activation.recall.candidates.size(); ++at) {
        const auto& candidate = activation.recall.candidates[at];
        if (!identifiers.insert(candidate.episode_id).second)
            throw std::runtime_error("activation stages repeat an original");
        const auto& replayed = activation.replay.episodes[at];
        const auto& verdict = activation.re_evidence.judgments[at];
        const auto episode = pair.memory().episode(candidate.episode_id);
        if (episode.episode_id != candidate.episode_id)
            throw std::runtime_error("activation original address changed");
        std::vector<std::string> outcomes;
        outcomes.reserve(episode.steps.size());
        for (const auto& step : episode.steps) outcomes.push_back(step.outcome);
        if (candidate.revision != episode.revision ||
            replayed.source_addresses != episode.source_addresses ||
            candidate.historical_outcomes != outcomes ||
            !same_original_steps(replayed.steps, episode.steps))
            throw std::runtime_error("activation original experience lineage changed");
        const auto address = nodes.address(episode.episode_id);
        const auto component = regions.component_for(address);
        if (!component) throw std::runtime_error("activated original has no region component");
        const auto topology = regions.topology_for(*component);
        if (!topology || topology->vrs_snapshot_id() != inputs.snapshot_id())
            throw std::runtime_error("region topology belongs to a different VRS generation");
        auto memberships = graph_episode_memberships(episode.episode_id, pair, inputs,
                                                       nodes, regions);
        topologies.insert(topology->topology_id());
        rows.push_back(CoactivatedExperience{
            episode.episode_id, episode.revision, episode.source_addresses,
            std::move(outcomes), topology->topology_id(), std::move(memberships),
            verdict.proposition, verdict.verdict});
    }
    std::optional<std::string> one_topology;
    if (topologies.size() == 1) one_topology = *topologies.begin();
    return CoactivationEvent{std::move(request_id), observed_at_ns,
        pair.snapshot_id(), pair.memory().snapshot_id(), pair.vrs_snapshot_id(),
        std::move(one_topology), activation.recall.query,
        activation.deja_vu.current_cues, std::move(rows),
        activation.re_evidence.should_abstain, false};
}

}  // namespace swegca::vrs
