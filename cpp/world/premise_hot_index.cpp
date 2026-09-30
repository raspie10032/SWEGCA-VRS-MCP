#include "world/premise_hot_index.hpp"

#include <algorithm>
#include <stdexcept>

namespace swegca::world {
namespace {

using EventKey = std::pair<std::string, std::size_t>;

SemanticSourceEpisode semantic_episode(const MemoryEpisode& episode) {
    std::vector<SemanticMemoryStep> steps;
    steps.reserve(episode.steps.size());
    for (const auto& step : episode.steps)
        steps.push_back({step.phase, JsonValue(step.observation), step.relations,
                         step.judgment, step.outcome, step.evidence_refs});
    return {episode.episode_id, episode.cues, std::move(steps),
            episode.source_addresses, episode.revision, episode.verification_state};
}

void append_unique(std::vector<std::string>& values, const std::string& value) {
    if (std::ranges::find(values, value) == values.end()) values.push_back(value);
}

SemanticFamilyDirectory families(
    const std::map<EventKey, RecordedEncodedEvent>& events) {
    SemanticFamilyDirectory result;
    for (const auto& [unused, event] : events) {
        static_cast<void>(unused);
        std::vector<std::string> parents;
        if (const auto* semantic = std::get_if<SemanticEncoding>(&event.semantic_encoding))
            parents.push_back(semantic->source_id);
        else if (const auto* session =
                     std::get_if<std::shared_ptr<const SessionContentEncoding>>(
                         &event.semantic_encoding); session && *session)
            parents = (*session)->source_episode_ids();
        if (std::ranges::find(event.unresolved, "semantic_source_binding_unresolved") !=
            event.unresolved.end()) continue;
        for (const auto& parent : parents) {
            append_unique(result.by_parent[parent], event.episode_id);
            append_unique(result.by_child[event.episode_id], parent);
        }
    }
    return result;
}

bool vrs_parent(const std::string_view identifier) {
    return std::ranges::any_of(vrs_virtual_episode_prefixes,
        [&](const auto prefix) { return identifier.starts_with(prefix); });
}

}  // namespace

struct PremiseHotMemoryIndex::Shard final {
    PremiseReadView::AddressMap by_address;
    PremiseReadView::PairMap by_pair;
    PremiseReadView::TermMap by_term;
    std::set<std::string, std::less<>> inspected;
    std::set<std::string, std::less<>> absent;
    std::set<std::string, std::less<>> virtual_ids;
    std::map<EventKey, RecordedEncodedEvent> recorded_events;
    SemanticFamilyDirectory semantic_families;
};

namespace {

std::shared_ptr<const PremiseHotMemoryIndex::Shard> prepare(
    const std::shared_ptr<const HotMemoryIndex>& base,
    const std::vector<std::string>& identifiers,
    const PreparedSessionView* source_memory = nullptr) {
    auto result = std::make_shared<PremiseHotMemoryIndex::Shard>();
    std::map<std::string, SemanticSourceEpisode, std::less<>> semantic_cache;
    const RecordedSourceLookup lookup = [&](const std::string_view identifier)
        -> const SemanticSourceEpisode* {
        const auto found = semantic_cache.find(identifier);
        if (found != semantic_cache.end()) return &found->second;
        try {
            auto [at, inserted] = semantic_cache.emplace(
                std::string(identifier), semantic_episode(base->episode(identifier)));
            static_cast<void>(inserted);
            return &at->second;
        } catch (const std::out_of_range&) { return nullptr; }
    };
    const auto admitted = [&](const MemoryEpisode& episode) {
        auto source = semantic_episode(episode);
        semantic_cache.insert_or_assign(source.episode_id, source);
        for (std::size_t ordinal = 0; ordinal < source.steps.size(); ++ordinal) {
            auto event = prepare_recorded_event(source, ordinal, lookup, source_memory);
            if (event) result->recorded_events.emplace(
                EventKey{episode.episode_id, ordinal}, std::move(*event));
        }
    };
    const auto segment = prepare_premise_segment(base, identifiers, admitted);
    result->by_address = *segment.address_index();
    result->by_pair = *segment.pair_index();
    result->by_term = *segment.term_index();
    result->inspected.insert(segment.inspected_episode_ids.begin(),
                             segment.inspected_episode_ids.end());
    result->absent.insert(segment.episodes_without_explicit_premises.begin(),
                          segment.episodes_without_explicit_premises.end());
    for (const auto& id : result->inspected) if (vrs_parent(id)) result->virtual_ids.insert(id);
    result->semantic_families = families(result->recorded_events);
    return result;
}

std::shared_ptr<const PremiseHotMemoryIndex::Shard> merge(
    const PremiseHotMemoryIndex::Shard& left,
    const PremiseHotMemoryIndex::Shard& right) {
    auto result = std::make_shared<PremiseHotMemoryIndex::Shard>(left);
    for (const auto& id : right.inspected)
        if (result->inspected.contains(id))
            throw std::invalid_argument("premise generations overlap");
    result->by_address.insert(right.by_address.begin(), right.by_address.end());
    for (const auto& [key, values] : right.by_pair)
        result->by_pair[key].insert(result->by_pair[key].end(), values.begin(), values.end());
    for (const auto& [key, values] : right.by_term)
        result->by_term[key].insert(result->by_term[key].end(), values.begin(), values.end());
    result->inspected.insert(right.inspected.begin(), right.inspected.end());
    result->absent.insert(right.absent.begin(), right.absent.end());
    result->virtual_ids.insert(right.virtual_ids.begin(), right.virtual_ids.end());
    result->recorded_events.insert(right.recorded_events.begin(), right.recorded_events.end());
    result->semantic_families = result->semantic_families.merge(right.semantic_families);
    return result;
}

std::size_t weight(const PremiseHotMemoryIndex::Shard& shard) {
    return shard.inspected.size() + shard.by_address.size();
}

}  // namespace

PremiseHotMemoryIndex::PremiseHotMemoryIndex(
    std::shared_ptr<const HotMemoryIndex> base_value,
    std::vector<std::shared_ptr<const Shard>> shard_values,
    SessionDocumentDirectory documents, PreparedSessionCache cache)
    : base(std::move(base_value)), shards(std::move(shard_values)),
      session_documents(std::move(documents)), prepared_sessions(std::move(cache)) {
    if (!base || base->lookup_requires_io())
        throw std::invalid_argument("unwrapped hot base required");
}

std::shared_ptr<const PremiseHotMemoryIndex> PremiseHotMemoryIndex::build(
    std::shared_ptr<const HotMemoryIndex> base,
    const std::vector<std::string>& prepared) {
    if (!base || base->lookup_requires_io() ||
        std::dynamic_pointer_cast<const PremiseHotMemoryIndex>(base))
        throw std::invalid_argument("unwrapped hot base required");
    std::vector<std::shared_ptr<const Shard>> shards;
    if (!prepared.empty()) shards.push_back(prepare(base, prepared));
    return std::shared_ptr<const PremiseHotMemoryIndex>(new PremiseHotMemoryIndex(
        std::move(base), std::move(shards), {}, {}));
}

std::string_view PremiseHotMemoryIndex::snapshot_id() const noexcept { return base->snapshot_id(); }
std::size_t PremiseHotMemoryIndex::episode_count() const noexcept { return base->episode_count(); }
const std::map<std::string, std::size_t, std::less<>>&
PremiseHotMemoryIndex::outcome_counts() const noexcept { return base->outcome_counts(); }
const MemoryEpisode& PremiseHotMemoryIndex::episode(const std::string_view id) const {
    return base->episode(id);
}
std::vector<std::string> PremiseHotMemoryIndex::episode_ids_for_cue(
    const std::string_view cue) const { return base->episode_ids_for_cue(cue); }
std::vector<std::string> PremiseHotMemoryIndex::iter_episode_ids() const {
    return base->iter_episode_ids();
}
std::vector<SemanticFamilyDirectory> PremiseHotMemoryIndex::semantic_family_directories() const {
    auto result = base->semantic_family_directories();
    for (const auto& shard : shards) result.push_back(shard->semantic_families);
    return result;
}

std::string PremiseHotMemoryIndex::preparation_state(const std::string_view id) const {
    for (auto at = shards.rbegin(); at != shards.rend(); ++at)
        if ((*at)->inspected.contains(id))
            return (*at)->absent.contains(id) ? "no_explicit_premises" : "prepared";
    return "unprepared";
}

PremiseReadView PremiseHotMemoryIndex::premise_view() const {
    auto addresses = std::make_shared<PremiseReadView::AddressMap>();
    auto pairs = std::make_shared<PremiseReadView::PairMap>();
    auto terms = std::make_shared<PremiseReadView::TermMap>();
    for (const auto& shard : shards) {
        addresses->insert(shard->by_address.begin(), shard->by_address.end());
        for (const auto& [key, values] : shard->by_pair)
            (*pairs)[key].insert((*pairs)[key].end(), values.begin(), values.end());
        for (const auto& [key, values] : shard->by_term)
            (*terms)[key].insert((*terms)[key].end(), values.begin(), values.end());
    }
    return PremiseReadView::from_indexes(std::string(snapshot_id()),
        std::move(addresses), std::move(pairs), std::move(terms));
}

std::optional<RecordedEncodedEvent> PremiseHotMemoryIndex::recorded_event(
    const std::string_view identifier, const std::size_t step) const {
    for (auto at = shards.rbegin(); at != shards.rend(); ++at) {
        const auto found = (*at)->recorded_events.find({std::string(identifier), step});
        if (found != (*at)->recorded_events.end()) return found->second;
    }
    return std::nullopt;
}

SessionDocumentDirectoryView PremiseHotMemoryIndex::session_document_view() const {
    return session_documents.bind(std::string(snapshot_id()));
}
PreparedSessionView PremiseHotMemoryIndex::prepared_session_view() const {
    return prepared_sessions.bind(std::string(snapshot_id()), session_document_view());
}

std::shared_ptr<const PremiseHotMemoryIndex> PremiseHotMemoryIndex::append(
    const std::vector<MemoryEpisode>& episodes,
    const std::vector<std::string>& required_outcomes) const {
    auto replacement = append_memory_activation_index(base, episodes, required_outcomes);
    if (episodes.empty()) return shared_from_this();
    std::vector<SemanticSourceEpisode> semantic;
    std::vector<std::string> identifiers;
    for (const auto& episode : episodes) {
        semantic.push_back(semantic_episode(episode));
        identifiers.push_back(episode.episode_id);
    }
    auto [documents, changed] = session_documents.append_with_keys(semantic);
    auto cache = prepared_sessions.invalidate(changed);
    auto source_memory = std::shared_ptr<const PremiseHotMemoryIndex>(new PremiseHotMemoryIndex(
        replacement, shards, documents, cache));
    auto view = source_memory->prepared_session_view();
    auto result = shards;
    result.push_back(prepare(replacement, identifiers, &view));
    while (result.size() > 1 && 2 * weight(*result.back()) >= weight(*result[result.size() - 2])) {
        auto combined = merge(*result[result.size() - 2], *result.back());
        result.pop_back(); result.pop_back(); result.push_back(std::move(combined));
    }
    return std::shared_ptr<const PremiseHotMemoryIndex>(new PremiseHotMemoryIndex(
        std::move(replacement), std::move(result), std::move(documents), std::move(cache)));
}

std::pair<std::shared_ptr<const PremiseHotMemoryIndex>, std::size_t>
PremiseHotMemoryIndex::after_vrs_rebind(
    std::shared_ptr<const HotMemoryIndex> replacement) const {
    auto old_base = base;
    auto new_base = replacement;
    if (const auto old_atoms = std::dynamic_pointer_cast<const AtomHotMemoryIndex>(old_base)) {
        const auto new_atoms = std::dynamic_pointer_cast<const AtomHotMemoryIndex>(new_base);
        if (!new_atoms) throw std::invalid_argument("atom source wrapper lost during premise rebind");
        old_base = old_atoms->base;
        new_base = new_atoms->base;
    }
    const auto bound = std::dynamic_pointer_cast<const VrsGenerationBoundMemoryIndex>(new_base);
    if (!bound) throw std::invalid_argument("validated VRS generation rebind required");
    std::vector<std::shared_ptr<const HotMemoryIndex>> ordinary;
    for (const auto& leaf : vrs_leaf_sources(old_base))
        if (!std::dynamic_pointer_cast<const VrsHotMemorySource>(leaf)) ordinary.push_back(leaf);
    if (ordinary.size() != bound->ordinary_sources.size())
        throw std::invalid_argument("ordinary source identity changed during premise rebind");
    for (std::size_t index = 0; index < ordinary.size(); ++index)
        if (ordinary[index].get() != bound->ordinary_sources[index].get())
            throw std::invalid_argument("ordinary source identity changed during premise rebind");
    std::vector<std::shared_ptr<const Shard>> kept;
    std::size_t invalidated{};
    for (const auto& shard : shards) {
        invalidated += shard->virtual_ids.size();
        if (shard->virtual_ids.empty()) { kept.push_back(shard); continue; }
        if (shard->virtual_ids.size() == shard->inspected.size()) continue;
        auto filtered = std::make_shared<Shard>();
        for (const auto& [address, premise] : shard->by_address)
            if (!shard->virtual_ids.contains(address.episode_id))
                filtered->by_address.emplace(address, premise);
        for (const auto& [key, values] : shard->by_pair)
            for (const auto& address : values)
                if (!shard->virtual_ids.contains(address.episode_id))
                    filtered->by_pair[key].push_back(address);
        for (const auto& [key, values] : shard->by_term)
            for (const auto& address : values)
                if (!shard->virtual_ids.contains(address.episode_id))
                    filtered->by_term[key].push_back(address);
        for (const auto& id : shard->inspected)
            if (!shard->virtual_ids.contains(id)) filtered->inspected.insert(id);
        for (const auto& id : shard->absent)
            if (!shard->virtual_ids.contains(id)) filtered->absent.insert(id);
        for (const auto& [key, event] : shard->recorded_events)
            if (!shard->virtual_ids.contains(key.first)) filtered->recorded_events.emplace(key, event);
        filtered->semantic_families = families(filtered->recorded_events);
        kept.push_back(std::move(filtered));
    }
    return {std::shared_ptr<const PremiseHotMemoryIndex>(new PremiseHotMemoryIndex(
                std::move(replacement), std::move(kept), session_documents, prepared_sessions)),
            invalidated};
}

}  // namespace swegca::world
