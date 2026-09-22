#include "event_delta.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <span>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

namespace swegca::vrs {
namespace {

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_delta.py@7536139:132-133
bool generation_digest(std::string_view identifier) {
    return identifier.size() == 64 &&
        std::all_of(identifier.begin(), identifier.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        });
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_delta.py@7536139:59-66
template <typename T>
void validate_edits(const std::vector<std::pair<std::uint32_t, T>>& edits,
                    std::uint64_t original_size, bool nonnegative = false) {
    std::set<std::uint32_t> seen;
    for (const auto& [address, value] : edits) {
        if (address >= original_size || !seen.insert(address).second)
            throw std::runtime_error("event delta shape, dtype or address changed");
        if constexpr (std::is_floating_point_v<T>) {
            if (!std::isfinite(value) || (nonnegative && value < 0))
                throw std::runtime_error("event delta contains invalid numeric edit");
        } else if (value > 1) {
            throw std::runtime_error("event delta unresolved value changed");
        }
    }
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_delta.py@7536139:65-66
void validate_float_append(const std::vector<float>& values, bool nonnegative = false) {
    for (const float value : values)
        if (!std::isfinite(value) || (nonnegative && value < 0))
            throw std::runtime_error("event delta contains invalid numeric append");
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_delta.py@7536139:145-155
void validate_delta(const EventVrsInputView& parent,
                    const EventDeltaChanges& changes) {
    const auto added_nodes = changes.appended_direct.size();
    const auto added_edges = changes.appended_edges.size();
    if (changes.appended_score.size() != added_nodes ||
        changes.appended_unresolved.size() != added_nodes ||
        changes.appended_strength.size() != added_edges ||
        parent.node_count() + added_nodes > std::uint64_t{0x100000000ULL} ||
        parent.edge_count() + added_edges > std::numeric_limits<std::uint32_t>::max())
        throw std::runtime_error("event delta node/edge counts disagree");
    validate_edits(changes.base_edits, parent.edge_count(), true);
    validate_edits(changes.strength_edits, parent.edge_count(), true);
    validate_edits(changes.direct_edits, parent.node_count());
    validate_edits(changes.score_edits, parent.node_count());
    validate_edits(changes.unresolved_edits, parent.node_count());
    validate_float_append(changes.appended_direct);
    validate_float_append(changes.appended_score);
    validate_float_append(changes.appended_strength, true);
    for (const auto value : changes.appended_unresolved)
        if (value > 1) throw std::runtime_error("event delta unresolved value changed");
    const auto node_count = parent.node_count() + added_nodes;
    for (const auto& edge : changes.appended_edges)
        if (!std::isfinite(edge.vrs_strength) || edge.vrs_strength < 0 ||
            (edge.sign != -1 && edge.sign != 0 && edge.sign != 1) ||
            edge.source >= node_count || edge.target >= node_count)
            throw std::runtime_error("event delta edge values or endpoints changed");
}

}  // namespace

// SWEGCA: src/swegca_vrs2/store.py@7536139:185-191
std::shared_ptr<const ValidatedEventVrsInputs> EventDeltaView::empty(
    std::string snapshot_id) {
    auto source = std::shared_ptr<EventDeltaView>(new EventDeltaView());
    source->snapshot_id_ = std::move(snapshot_id);
    source->dependencies_ = SegmentedEndpointDependencyIndex::build_empty(*source);
    return std::shared_ptr<const ValidatedEventVrsInputs>(
        new ValidatedEventVrsInputs(std::move(source)));
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_delta.py@7536139:117-180
std::shared_ptr<const ValidatedEventVrsInputs> EventDeltaView::prepare(
    std::shared_ptr<const ValidatedEventVrsInputs> parent,
    std::string snapshot_id, const EventDeltaChanges& changes) {
    if (!parent) throw std::runtime_error("cold-bound event parent required");
    const auto& original = parent->require_validated_immutable();
    if (!generation_digest(snapshot_id))
        throw std::runtime_error("event inputs need a generation digest");
    validate_delta(original, changes);
    const auto& old_index = original.dependencies();
    auto successor = std::shared_ptr<EventDeltaView>(new EventDeltaView());
    successor->snapshot_id_ = std::move(snapshot_id);
    successor->node_count_ = original.node_count() + changes.appended_direct.size();
    successor->edge_count_ = original.edge_count() + changes.appended_edges.size();
    successor->delta_parent_ = parent;
    if (const auto previous = std::dynamic_pointer_cast<const EventDeltaView>(parent->source_)) {
        successor->base_ = previous->base_;
        successor->direct_ = previous->direct_;
        successor->score_ = previous->score_;
        successor->unresolved_ = previous->unresolved_;
        successor->edge_ = previous->edge_;
        successor->strength_ = previous->strength_;
    } else successor->base_ = parent->source_;

    // Changed old addresses are copied into the persistent path. Appended
    // addresses occupy the exact old-size prefix; no old endpoint/sign edit
    // entry exists in this constructor.
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_delta.py@7536139:138-163
    for (const auto& [at, value] : changes.direct_edits)
        successor->direct_ = successor->direct_.with(at, value);
    for (const auto& [at, value] : changes.score_edits)
        successor->score_ = successor->score_.with(at, value);
    for (const auto& [at, value] : changes.unresolved_edits)
        successor->unresolved_ = successor->unresolved_.with(at, value);
    for (const auto& [at, value] : changes.strength_edits)
        successor->strength_ = successor->strength_.with(at, value);
    for (const auto& [at, value] : changes.base_edits) {
        auto edge = original.edge(at);
        edge.vrs_strength = value;
        successor->edge_ = successor->edge_.with(at, edge);
    }
    for (std::size_t at = 0; at < changes.appended_direct.size(); ++at) {
        const auto address = static_cast<std::uint32_t>(original.node_count() + at);
        successor->direct_ = successor->direct_.with(address, changes.appended_direct[at]);
        successor->score_ = successor->score_.with(address, changes.appended_score[at]);
        successor->unresolved_ = successor->unresolved_.with(address,
                                                              changes.appended_unresolved[at]);
    }
    for (std::size_t at = 0; at < changes.appended_edges.size(); ++at) {
        const auto address = static_cast<std::uint32_t>(original.edge_count() + at);
        successor->edge_ = successor->edge_.with(address, changes.appended_edges[at]);
        successor->strength_ = successor->strength_.with(address,
                                                          changes.appended_strength[at]);
    }
    successor->dependencies_ = old_index.extend_verified(
        *successor, std::span<const EventEdge>(changes.appended_edges));
    return std::shared_ptr<const ValidatedEventVrsInputs>(
        new ValidatedEventVrsInputs(std::move(successor), *parent,
            changes.appended_direct.size(), changes.appended_edges.size()));
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_delta.py@7536139:84-94
float EventDeltaView::direct(std::uint32_t node) const {
    if (node >= node_count_) throw std::out_of_range("event numeric address outside directory");
    if (const auto value = direct_.get(node)) return *value;
    if (base_ && node < base_->node_count()) return base_->direct(node);
    throw std::runtime_error("event direct address has no backing");
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_delta.py@7536139:84-94
float EventDeltaView::score(std::uint32_t node) const {
    if (node >= node_count_) throw std::out_of_range("event numeric address outside directory");
    if (const auto value = score_.get(node)) return *value;
    if (base_ && node < base_->node_count()) return base_->score(node);
    throw std::runtime_error("event score address has no backing");
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_delta.py@7536139:84-94
bool EventDeltaView::unresolved(std::uint32_t node) const {
    if (node >= node_count_) throw std::out_of_range("event numeric address outside directory");
    if (const auto value = unresolved_.get(node)) return *value != 0;
    if (base_ && node < base_->node_count()) return base_->unresolved(node);
    throw std::runtime_error("event unresolved address has no backing");
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_delta.py@7536139:157-163
EventEdge EventDeltaView::edge(std::uint32_t address) const {
    if (address >= edge_count_) throw std::out_of_range("event edge address outside directory");
    if (const auto value = edge_.get(address)) return *value;
    if (base_ && address < base_->edge_count()) return base_->edge(address);
    throw std::runtime_error("event edge address has no backing");
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_delta.py@7536139:84-94
float EventDeltaView::strength(std::uint32_t address) const {
    if (address >= edge_count_) throw std::out_of_range("event strength address outside directory");
    if (const auto value = strength_.get(address)) return *value;
    if (base_ && address < base_->edge_count()) return base_->strength(address);
    throw std::runtime_error("event strength address has no backing");
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_delta.py@7536139:75-79
void EventDeltaView::require_immutable_binding() const {
    if (base_) base_->require_immutable_binding();
    if (!dependencies_) throw std::runtime_error("event dependency binding missing");
    dependencies_->require_source(*this);
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_delta.py@7536139:164-175
const EndpointDependencyIndex& EventDeltaView::dependencies() const {
    if (!dependencies_) throw std::runtime_error("event dependency binding missing");
    return *dependencies_;
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_delta.py@7536139:117-180
std::shared_ptr<const ValidatedEventVrsInputs> prepare_event_delta(
    std::shared_ptr<const ValidatedEventVrsInputs> parent,
    std::string snapshot_id, const EventDeltaChanges& changes) {
    return EventDeltaView::prepare(std::move(parent), std::move(snapshot_id), changes);
}

}  // namespace swegca::vrs
