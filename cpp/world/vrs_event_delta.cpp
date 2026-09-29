#include "world/vrs_event_delta.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

namespace swegca::world {
namespace {

bool digest(const std::string_view value) {
    return value.size() == 64 && std::ranges::all_of(value, [](const char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

template<class T>
void require_addresses(const std::span<const std::size_t> indices,
                       const std::span<const T> values,
                       const std::size_t size) {
    if (indices.size() != values.size())
        throw std::invalid_argument("event delta shape, dtype or address changed");
    std::set<std::size_t> unique;
    for (const auto index : indices)
        if (index >= size || !unique.insert(index).second)
            throw std::invalid_argument("event delta shape, dtype or address changed");
}

void require_finite(const std::span<const float> values, const bool nonnegative = false) {
    for (const auto value : values)
        if (!std::isfinite(value) || (nonnegative && value < 0))
            throw std::invalid_argument("event delta contains invalid numeric values");
}

}  // namespace

std::shared_ptr<const EventSignalInputs> prepare_event_delta(
    const std::shared_ptr<const EventSignalInputs>& parent,
    std::string snapshot_id,
    const std::span<const float> appended_direct,
    const std::span<const float> appended_score,
    const std::span<const std::uint8_t> appended_unresolved,
    const std::span<const EventSignalEdge> appended_edges,
    const std::span<const float> appended_strength,
    const std::span<const std::size_t> base_indices,
    const std::span<const float> base_values,
    const std::span<const std::size_t> strength_indices,
    const std::span<const float> strength_values,
    const std::span<const std::size_t> direct_indices,
    const std::span<const float> direct_values,
    const std::span<const std::size_t> score_indices,
    const std::span<const float> score_values,
    const std::span<const std::size_t> unresolved_indices,
    const std::span<const std::uint8_t> unresolved_values) {
    if (!parent || !digest(snapshot_id) || appended_direct.size() != appended_score.size() ||
        appended_direct.size() != appended_unresolved.size() ||
        appended_edges.size() != appended_strength.size())
        throw std::invalid_argument("event delta shape or generation changed");
    require_addresses(base_indices, base_values, parent->edges.size());
    require_addresses(strength_indices, strength_values, parent->strength.size());
    require_addresses(direct_indices, direct_values, parent->direct.size());
    require_addresses(score_indices, score_values, parent->score.size());
    require_addresses(unresolved_indices, unresolved_values, parent->unresolved.size());
    require_finite(appended_direct);
    require_finite(appended_score);
    require_finite(appended_strength, true);
    require_finite(base_values, true);
    require_finite(strength_values, true);
    require_finite(direct_values);
    require_finite(score_values);
    if (std::ranges::any_of(appended_unresolved, [](const auto v) { return v > 1; }) ||
        std::ranges::any_of(unresolved_values, [](const auto v) { return v > 1; }))
        throw std::invalid_argument("event delta unresolved value changed");

    const auto node_count = parent->score.size() + appended_score.size();
    for (const auto& edge : appended_edges)
        if (edge.source >= node_count || edge.target >= node_count || edge.sign < -1 ||
            edge.sign > 1 || !std::isfinite(edge.vrs_strength) || edge.vrs_strength < 0)
            throw std::invalid_argument("event delta edge values or endpoints changed");

    std::vector<EventSignalEdge> changed_edges;
    changed_edges.reserve(base_indices.size());
    for (std::size_t i = 0; i < base_indices.size(); ++i) {
        auto edge = parent->edges[base_indices[i]];
        edge.vrs_strength = base_values[i];
        changed_edges.push_back(edge);
    }
    auto direct = PersistentEventVector<float>::extend(
        parent->direct, direct_indices, direct_values, appended_direct);
    auto score = PersistentEventVector<float>::extend(
        parent->score, score_indices, score_values, appended_score);
    auto unresolved = PersistentEventVector<std::uint8_t>::extend(
        parent->unresolved, unresolved_indices, unresolved_values, appended_unresolved);
    auto strength = PersistentEventVector<float>::extend(
        parent->strength, strength_indices, strength_values, appended_strength);
    auto edges = PersistentEventVector<EventSignalEdge>::extend(
        parent->edges, base_indices, changed_edges, appended_edges);
    if (strength.size() != edges.size() || strength.size() > 0xffffffffULL)
        throw std::invalid_argument("event delta node/edge counts disagree");
    return std::shared_ptr<const EventSignalInputs>(new EventSignalInputs(
        std::move(snapshot_id), std::move(direct), std::move(score), std::move(edges),
        std::move(strength), std::move(unresolved), parent,
        std::vector<std::size_t>(score_indices.begin(), score_indices.end()),
        std::vector<std::size_t>(strength_indices.begin(), strength_indices.end()),
        std::vector<std::size_t>(direct_indices.begin(), direct_indices.end())));
}

}  // namespace swegca::world
