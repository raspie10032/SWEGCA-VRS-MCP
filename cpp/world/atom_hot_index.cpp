#include "world/atom_hot_index.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>

namespace swegca::world {
namespace {

struct Route final {
    std::string parent;
    std::string kind;
    std::variant<std::size_t, std::string> index;
};

bool vrs_parent(const std::string_view identifier) {
    return std::ranges::any_of(vrs_virtual_episode_prefixes,
        [&](const auto prefix) { return identifier.starts_with(prefix); });
}

}  // namespace

struct AtomHotMemoryIndex::Segment final {
    std::map<std::string, AtomHotSidecar, std::less<>> parents;
    std::map<std::string, Route, std::less<>> routes;
    std::set<std::string, std::less<>> vrs_parent_ids;
};

namespace {

std::shared_ptr<const AtomHotMemoryIndex::Segment> segment(
    const HotMemoryIndex& base, const std::vector<std::string>& identifiers,
    const std::size_t maximum_bytes) {
    auto result = std::make_shared<AtomHotMemoryIndex::Segment>();
    for (const auto& identifier : identifiers) {
        if (result->parents.contains(identifier))
            throw std::invalid_argument("duplicate prepared parent");
        auto binding = std::make_shared<const EpisodeAtoms>(
            EpisodeAtoms::build(base.episode(identifier), maximum_bytes));
        auto media = std::make_shared<const EpisodeMediaSelectors>(
            EpisodeMediaSelectors::build(*binding));
        auto flow = std::make_shared<const EpisodeFlow>(EpisodeFlow(*media));
        result->parents.emplace(identifier, AtomHotSidecar{binding, media, flow});
        if (vrs_parent(identifier)) result->vrs_parent_ids.insert(identifier);
        for (const auto& [atom_id, unused] : binding->locations) {
            static_cast<void>(unused);
            if (!result->routes.emplace(atom_id, Route{identifier, "byte", atom_id}).second)
                throw std::invalid_argument("duplicate atom address");
        }
        for (std::size_t index = 0; index < media->selectors.size(); ++index) {
            const auto& atom_id = media->selectors[index].atom_id;
            if (!result->routes.emplace(atom_id, Route{identifier, "media", index}).second)
                throw std::invalid_argument("duplicate atom address");
        }
        for (const auto& [address, unused] : flow->nodes) {
            static_cast<void>(unused);
            if (!result->routes.emplace(address, Route{identifier, "flow", address}).second)
                throw std::invalid_argument("duplicate flow address");
        }
    }
    return result;
}

std::shared_ptr<const AtomHotMemoryIndex::Segment> merge(
    const AtomHotMemoryIndex::Segment& left,
    const AtomHotMemoryIndex::Segment& right) {
    auto result = std::make_shared<AtomHotMemoryIndex::Segment>();
    result->parents = left.parents;
    result->routes = left.routes;
    result->vrs_parent_ids = left.vrs_parent_ids;
    for (const auto& [key, value] : right.parents)
        if (!result->parents.emplace(key, value).second)
            throw std::invalid_argument("sidecar generations overlap");
    for (const auto& [key, value] : right.routes)
        if (!result->routes.emplace(key, value).second)
            throw std::invalid_argument("sidecar generations overlap");
    result->vrs_parent_ids.insert(right.vrs_parent_ids.begin(), right.vrs_parent_ids.end());
    return result;
}

}  // namespace

AtomHotMemoryIndex::AtomHotMemoryIndex(
    std::shared_ptr<const HotMemoryIndex> base_value,
    std::vector<std::shared_ptr<const Segment>> segment_values,
    const std::size_t bytes)
    : base(std::move(base_value)), segments(std::move(segment_values)),
      maximum_bytes(bytes) {
    if (!base || base->lookup_requires_io())
        throw std::invalid_argument("unwrapped hot base required");
    if (maximum_bytes < 4) throw std::invalid_argument("atom granularity must be >= 4");
}

std::shared_ptr<const AtomHotMemoryIndex> AtomHotMemoryIndex::build(
    std::shared_ptr<const HotMemoryIndex> base,
    const std::vector<std::string>& prepared, const std::size_t maximum_bytes) {
    if (!base || base->lookup_requires_io() ||
        std::dynamic_pointer_cast<const AtomHotMemoryIndex>(base))
        throw std::invalid_argument("unwrapped hot base required");
    std::vector<std::shared_ptr<const Segment>> segments;
    if (!prepared.empty()) segments.push_back(segment(*base, prepared, maximum_bytes));
    return std::shared_ptr<const AtomHotMemoryIndex>(
        new AtomHotMemoryIndex(std::move(base), std::move(segments), maximum_bytes));
}

std::string_view AtomHotMemoryIndex::snapshot_id() const noexcept { return base->snapshot_id(); }
std::size_t AtomHotMemoryIndex::episode_count() const noexcept { return base->episode_count(); }
const std::map<std::string, std::size_t, std::less<>>&
AtomHotMemoryIndex::outcome_counts() const noexcept { return base->outcome_counts(); }
const MemoryEpisode& AtomHotMemoryIndex::episode(const std::string_view id) const {
    return base->episode(id);
}
std::vector<std::string> AtomHotMemoryIndex::episode_ids_for_cue(
    const std::string_view cue) const { return base->episode_ids_for_cue(cue); }
std::vector<std::string> AtomHotMemoryIndex::iter_episode_ids() const {
    return base->iter_episode_ids();
}

const AtomHotSidecar& AtomHotMemoryIndex::sidecar(const std::string_view identifier) const {
    (void)base->episode(identifier);
    for (auto at = segments.rbegin(); at != segments.rend(); ++at) {
        const auto found = (*at)->parents.find(identifier);
        if (found != (*at)->parents.end()) return found->second;
    }
    throw AtomPreparationRequired(std::string(identifier));
}

std::vector<std::string> AtomHotMemoryIndex::addresses(
    const std::string_view identifier) const {
    const auto& value = sidecar(identifier);
    std::vector<std::string> result;
    result.reserve(value.binding->locations.size() + value.media->selectors.size() +
                   value.flow->nodes.size());
    for (const auto& [address, unused] : value.binding->locations) {
        static_cast<void>(unused); result.push_back(address);
    }
    for (const auto& selector : value.media->selectors) result.push_back(selector.atom_id);
    for (const auto& [address, unused] : value.flow->nodes) {
        static_cast<void>(unused); result.push_back(address);
    }
    return result;
}

PreparedHotAddress AtomHotMemoryIndex::prepare_address(const std::string_view address) const {
    for (auto at = segments.rbegin(); at != segments.rend(); ++at) {
        const auto route = (*at)->routes.find(address);
        if (route == (*at)->routes.end()) continue;
        const auto& sidecar = (*at)->parents.at(route->second.parent);
        if (route->second.kind == "byte") {
            const auto [step_index, atom] = sidecar.binding->prepare(
                std::get<std::string>(route->second.index));
            return {std::string(snapshot_id()), route->second.parent, "byte",
                    PreparedEpisodeAtom{std::string(snapshot_id()), sidecar.binding->episode,
                                        step_index, atom}};
        }
        if (route->second.kind == "media")
            return {std::string(snapshot_id()), route->second.parent, "media",
                    sidecar.media->prepare(std::get<std::size_t>(route->second.index))};
        return {std::string(snapshot_id()), route->second.parent, "flow",
                sidecar.flow->prepare(std::get<std::string>(route->second.index))};
    }
    throw std::out_of_range(std::string(address));
}

std::shared_ptr<const AtomHotMemoryIndex> AtomHotMemoryIndex::append(
    const std::vector<MemoryEpisode>& episodes,
    const std::vector<std::string>& required_outcomes) const {
    auto replacement = append_memory_activation_index(base, episodes, required_outcomes);
    if (episodes.empty()) return shared_from_this();
    std::vector<std::string> ids;
    ids.reserve(episodes.size());
    for (const auto& episode : episodes) ids.push_back(episode.episode_id);
    auto result = segments;
    result.push_back(segment(*replacement, ids, maximum_bytes));
    while (result.size() > 1 &&
           2 * result.back()->routes.size() >= result[result.size() - 2]->routes.size()) {
        auto combined = merge(*result[result.size() - 2], *result.back());
        result.pop_back(); result.pop_back(); result.push_back(std::move(combined));
    }
    return std::shared_ptr<const AtomHotMemoryIndex>(
        new AtomHotMemoryIndex(std::move(replacement), std::move(result), maximum_bytes));
}

std::pair<std::shared_ptr<const AtomHotMemoryIndex>, std::size_t>
AtomHotMemoryIndex::after_vrs_rebind(
    std::shared_ptr<const HotMemoryIndex> replacement) const {
    const auto bound = std::dynamic_pointer_cast<const VrsGenerationBoundMemoryIndex>(replacement);
    if (!bound) throw std::invalid_argument("validated VRS generation rebind required");
    std::vector<std::shared_ptr<const HotMemoryIndex>> ordinary;
    for (const auto& leaf : vrs_leaf_sources(base))
        if (!std::dynamic_pointer_cast<const VrsHotMemorySource>(leaf)) ordinary.push_back(leaf);
    if (ordinary.size() != bound->ordinary_sources.size())
        throw std::invalid_argument("ordinary source identity changed during sidecar rebind");
    for (std::size_t index = 0; index < ordinary.size(); ++index)
        if (ordinary[index].get() != bound->ordinary_sources[index].get())
            throw std::invalid_argument("ordinary source identity changed during sidecar rebind");
    std::vector<std::shared_ptr<const Segment>> kept;
    std::size_t invalidated{};
    for (const auto& existing : segments) {
        invalidated += existing->vrs_parent_ids.size();
        if (existing->vrs_parent_ids.empty()) { kept.push_back(existing); continue; }
        if (existing->vrs_parent_ids.size() == existing->parents.size()) continue;
        auto filtered = std::make_shared<Segment>();
        for (const auto& [id, value] : existing->parents)
            if (!existing->vrs_parent_ids.contains(id)) filtered->parents.emplace(id, value);
        for (const auto& [id, route] : existing->routes)
            if (!existing->vrs_parent_ids.contains(route.parent)) filtered->routes.emplace(id, route);
        kept.push_back(std::move(filtered));
    }
    return {std::shared_ptr<const AtomHotMemoryIndex>(
                new AtomHotMemoryIndex(std::move(replacement), std::move(kept), maximum_bytes)),
            invalidated};
}

}  // namespace swegca::world
