#include "world/vrs_event_hot_publication.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace swegca::world {
namespace {

bool same_values(const std::vector<double>& expected,
                 const PersistentEventVector<float>& actual) {
    if (expected.size() != actual.size()) return false;
    for (std::size_t i = 0; i < expected.size(); ++i)
        if (expected[i] != static_cast<double>(actual[i])) return false;
    return true;
}

std::vector<double> doubles(const PersistentEventVector<float>& values) {
    std::vector<double> result;
    result.reserve(values.size());
    for (std::size_t i = 0; i < values.size(); ++i)
        result.push_back(static_cast<double>(values[i]));
    return result;
}

std::shared_ptr<const VrsHotMemorySource> rebuild_source(
    const VrsHotMemorySource& source,
    const BoundEventSignalStorage& storage,
    std::string address) {
    auto score = doubles(storage.inputs->score);
    auto strength = doubles(storage.inputs->strength);
    if (const auto* canonical = dynamic_cast<const CanonicalVrsHotMemorySource*>(&source))
        return std::make_shared<const CanonicalVrsHotMemorySource>(
            canonical->terms, std::move(score), canonical->support, canonical->refute,
            canonical->edge_source, canonical->edge_target, canonical->edge_sign,
            std::move(strength), canonical->evidence_requests, canonical->address_index,
            std::move(address), canonical->promotion_threshold,
            canonical->member_edge_to_group, canonical->group_member_offsets,
            canonical->group_member_edge_ids, canonical->member_manifest_address);
    return std::make_shared<const VrsHotMemorySource>(
        source.terms, std::move(score), source.support, source.refute,
        source.edge_source, source.edge_target, source.edge_sign, std::move(strength),
        source.evidence_requests, source.address_index, std::move(address),
        source.promotion_threshold);
}

}  // namespace

BoundEventHotSource::BoundEventHotSource(
    FullCurrentMemoryVrsSnapshot pair_value,
    std::shared_ptr<const VrsHotMemorySource> source_value,
    std::shared_ptr<const BoundEventSignalStorage> storage_value,
    std::shared_ptr<const VrsHotMemorySource> shape_template)
    : pair(std::move(pair_value)), source(std::move(source_value)),
      storage(std::move(storage_value)), shape_template_(std::move(shape_template)) {}

std::shared_ptr<const BoundEventHotSource> BoundEventHotSource::cold_bind(
    FullCurrentMemoryVrsSnapshot pair,
    std::shared_ptr<const VrsHotMemorySource> source,
    std::shared_ptr<const BoundEventSignalStorage> storage) {
    if (!source || !storage || pair.memory->snapshot_id() != storage->inputs->snapshot_id ||
        source->source_address != "vrs-report-sha256:" + pair.vrs_snapshot_id)
        throw std::invalid_argument("event hot source must belong to the current main pair");
    std::vector<std::shared_ptr<const VrsHotMemorySource>> leaves;
    for (const auto& leaf : vrs_leaf_sources(pair.memory))
        if (const auto candidate = std::dynamic_pointer_cast<const VrsHotMemorySource>(leaf))
            leaves.push_back(candidate);
    if (leaves.size() != 1 || leaves.front().get() != source.get())
        throw std::invalid_argument("event hot source must be the current main source object");
    if (!same_values(source->score, storage->inputs->score) ||
        !same_values(source->vrs_strength, storage->inputs->strength) ||
        source->terms.size() != storage->inputs->score.size() ||
        source->edge_source.size() != storage->inputs->edges.size())
        throw std::invalid_argument("event hot source numerical values differ");
    for (std::size_t i = 0; i < source->edge_source.size(); ++i) {
        const auto& edge = storage->inputs->edges[i];
        if (source->edge_source[i] != edge.source || source->edge_target[i] != edge.target ||
            source->edge_sign[i] != edge.sign)
            throw std::invalid_argument("event hot source topology differs");
    }
    return std::shared_ptr<const BoundEventHotSource>(new BoundEventHotSource(
        std::move(pair), source, std::move(storage), std::move(source)));
}

PreparedEventHotGeneration BoundEventHotSource::prepare(
    const PreparedEventSignalStorage& candidate, std::string vrs_snapshot_id) const {
    if (candidate.parent.get() != storage.get() ||
        candidate.proposal->inputs_owner.get() != storage->inputs.get())
        throw std::invalid_argument("numeric publication requires the same bound topology generation");
    auto successor = candidate.successor_inputs(vrs_snapshot_id);
    return prepare_storage(std::move(successor), std::move(vrs_snapshot_id));
}

PreparedEventHotGeneration BoundEventHotSource::prepare_storage(
    std::shared_ptr<const BoundEventSignalStorage> successor,
    std::string vrs_snapshot_id) const {
    if (!successor || !shape_template_ || successor->inputs->score.size() != source->terms.size() ||
        successor->inputs->strength.size() != source->edge_source.size())
        throw std::invalid_argument("numeric publication changed VRS topology");
    auto replacement = rebuild_source(*shape_template_, *successor,
        "vrs-report-sha256:" + vrs_snapshot_id);
    FullCurrentMemoryVrsSnapshot provisional(pair.memory, vrs_snapshot_id);
    auto rebound = rebind_full_current_vrs_source(
        provisional, replacement, vrs_snapshot_id);
    auto bound_storage = successor->rebind_snapshot(
        std::string(rebound.pair.memory->snapshot_id()));
    auto current = ResidentVrsStrengthIndex::current(replacement, vrs_snapshot_id);
    auto next = std::shared_ptr<const BoundEventHotSource>(new BoundEventHotSource(
        rebound.pair, replacement, bound_storage, replacement));
    EventHotPublicationReceipt receipt;
    receipt.rebind = std::move(rebound.receipt);
    receipt.bound_parent_pair_snapshot_id = pair.snapshot_id;
    return {std::move(rebound.pair), std::move(replacement), std::move(bound_storage),
        std::move(current), std::move(receipt), std::move(next)};
}

}  // namespace swegca::world
