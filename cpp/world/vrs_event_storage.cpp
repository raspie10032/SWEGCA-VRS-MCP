#include "world/vrs_event_storage.hpp"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>
#include <stdexcept>

namespace swegca::world {
namespace {

[[noreturn]] void reject(const char* message) { throw std::invalid_argument(message); }

std::vector<std::byte> float32_bytes(const std::span<const float> values) {
    static_assert(std::endian::native == std::endian::little);
    const auto bytes = std::as_bytes(values);
    return {bytes.begin(), bytes.end()};
}

std::vector<std::byte> float16_bytes(const std::span<const float> values) {
    std::vector<std::uint16_t> halves;
    halves.reserve(values.size());
    for (const auto value : values) halves.push_back(event_strength_float16_bits(value));
    const auto bytes = std::as_bytes(std::span(halves));
    return {bytes.begin(), bytes.end()};
}

std::vector<float> decode_float16(const std::span<const std::byte> bytes) {
    if (bytes.size() % 2) reject("invalid float16 storage bytes");
    std::vector<float> result;
    result.reserve(bytes.size() / 2);
    for (std::size_t offset = 0; offset < bytes.size(); offset += 2) {
        const auto half = static_cast<std::uint16_t>(std::to_integer<unsigned>(bytes[offset])) |
                          static_cast<std::uint16_t>(
                              std::to_integer<unsigned>(bytes[offset + 1]) << 8U);
        result.push_back(event_strength_from_float16_bits(half));
    }
    return result;
}

bool same(const float left, const float right) noexcept {
    return std::bit_cast<std::uint32_t>(left) == std::bit_cast<std::uint32_t>(right);
}

}  // namespace

StorageBoundEventSignalProposal::StorageBoundEventSignalProposal(
    EventSignalProposal signal_value,
    std::shared_ptr<const BoundEventSignalStorage> binding_value,
    std::shared_ptr<const EventSignalInputs> inputs_owner_value)
    : signal(std::move(signal_value)), binding(std::move(binding_value)),
      inputs_owner(std::move(inputs_owner_value)) {
    if (!binding || !inputs_owner || !signal.belongs_to(*inputs_owner) ||
        (inputs_owner != binding->inputs &&
         inputs_owner->delta_parent() != binding->inputs.get()))
        reject("event storage proposal binding changed");
}

PreparedEventSignalStorage::PreparedEventSignalStorage(
    std::shared_ptr<const BoundEventSignalStorage> parent_value,
    std::shared_ptr<const StorageBoundEventSignalProposal> proposal_value,
    std::shared_ptr<const VrsArrayBlocks> scores_value,
    std::shared_ptr<const VrsArrayBlocks> strengths_value,
    std::vector<VrsExperiencePromotionDecision> promotions_value,
    PreparedEventStorageDiagnostics diagnostics_value)
    : parent(std::move(parent_value)), proposal(std::move(proposal_value)),
      scores(std::move(scores_value)), strengths(std::move(strengths_value)),
      promotions(std::move(promotions_value)), diagnostics(diagnostics_value) {
    if (!parent || !proposal || proposal->binding != parent || !scores || !strengths)
        reject("invalid prepared event storage candidate");
}

BoundEventSignalStorage::BoundEventSignalStorage(
    std::shared_ptr<const EventSignalInputs> inputs_value,
    std::shared_ptr<const VrsArrayBlocks> scores_value,
    std::shared_ptr<const VrsArrayBlocks> strengths_value)
    : inputs(std::move(inputs_value)), scores(std::move(scores_value)),
      strengths(std::move(strengths_value)) {}

std::shared_ptr<const BoundEventSignalStorage> BoundEventSignalStorage::cold_bind(
    std::shared_ptr<const EventSignalInputs> inputs,
    std::shared_ptr<const VrsArrayBlocks> scores,
    std::shared_ptr<const VrsArrayBlocks> strengths) {
    if (!inputs || !scores || !strengths || scores->dtype != "<f4" ||
        strengths->dtype != "<f2" || scores->shape != std::vector<std::size_t>{inputs->score.size()} ||
        strengths->shape != std::vector<std::size_t>{inputs->strength.size()})
        reject("cold event storage layout changed");
    const auto dense_scores = inputs->score.materialize();
    if (scores->restore() != float32_bytes(dense_scores))
        reject("cold event score storage values differ from inputs");
    const auto restored_strength = decode_float16(strengths->restore());
    if (restored_strength.size() != inputs->strength.size())
        reject("cold event strength storage values differ from inputs");
    for (std::size_t i = 0; i < restored_strength.size(); ++i)
        if (!same(restored_strength[i], inputs->strength[i]))
            reject("cold event strength storage values differ from inputs");
    return std::shared_ptr<const BoundEventSignalStorage>(
        new BoundEventSignalStorage(std::move(inputs), std::move(scores), std::move(strengths)));
}

StorageBoundEventSignalProposal BoundEventSignalStorage::settle(
    const std::span<const std::size_t> changed_nodes,
    std::shared_ptr<const DetachedVrsStateUpdateReceipt> strength_updates,
    const std::string_view connection_namespace,
    const StorageBoundEventSignalProposal* previous,
    const std::uint64_t maximum_rounds) const {
    return settle_candidate(
        inputs, changed_nodes, std::move(strength_updates), connection_namespace,
        previous, maximum_rounds);
}

StorageBoundEventSignalProposal BoundEventSignalStorage::settle_candidate(
    std::shared_ptr<const EventSignalInputs> candidate,
    const std::span<const std::size_t> changed_nodes,
    std::shared_ptr<const DetachedVrsStateUpdateReceipt> strength_updates,
    const std::string_view connection_namespace,
    const StorageBoundEventSignalProposal* previous,
    const std::uint64_t maximum_rounds) const {
    if (!candidate || (candidate != inputs && candidate->delta_parent() != inputs.get()))
        reject("event storage parent generation changed");
    if (previous && previous->binding.get() != this)
        reject("event storage resume binding changed");

    std::vector<std::size_t> seeds(changed_nodes.begin(), changed_nodes.end());
    if (!previous && candidate != inputs) {
        std::set<std::size_t> automatic(seeds.begin(), seeds.end());
        std::set<std::size_t> strength_offsets(
            candidate->strength_indices().begin(), candidate->strength_indices().end());
        for (std::size_t edge = inputs->strength.size(); edge < candidate->strength.size(); ++edge)
            strength_offsets.insert(edge);
        for (const auto edge : strength_offsets) {
            const float value = candidate->strength[edge];
            const float durable = event_strength_from_float16_bits(
                event_strength_float16_bits(value));
            if (!std::isfinite(durable) || !same(value, durable))
                reject("event ingress strength must already match durable f16");
            automatic.insert(candidate->edges[edge].target);
        }
        for (std::size_t node = inputs->score.size(); node < candidate->score.size(); ++node)
            automatic.insert(node);
        automatic.insert(candidate->direct_indices().begin(), candidate->direct_indices().end());
        automatic.insert(candidate->score_indices().begin(), candidate->score_indices().end());
        for (const auto node : candidate->score_indices())
            for (const auto edge : candidate->outgoing(node))
                automatic.insert(candidate->edges[edge].target);
        seeds.assign(automatic.begin(), automatic.end());
    }

    const auto* prior_signal = previous ? &previous->signal : nullptr;
    auto result = settle_event_signal(
        *candidate, seeds, std::move(strength_updates), connection_namespace,
        prior_signal, maximum_rounds, EventStrengthStorage::float16);
    return StorageBoundEventSignalProposal(
        std::move(result), shared_from_this(), std::move(candidate));
}

PreparedEventSignalStorage BoundEventSignalStorage::prepare(
    const StorageBoundEventSignalProposal& proposal,
    const VrsBlockCodec codec) const {
    if (proposal.binding.get() != this || !proposal.inputs_owner ||
        !proposal.signal.belongs_to(*proposal.inputs_owner) ||
        (proposal.inputs_owner != inputs &&
         proposal.inputs_owner->delta_parent() != inputs.get()) ||
        proposal.signal.pending() ||
        proposal.signal.strength_storage != EventStrengthStorage::float16)
        reject("settled storage-bound event required");
    const auto& candidate = *proposal.inputs_owner;

    const auto score_count = inputs->score.size();
    std::set<std::size_t> changed_scores(
        candidate.score_indices().begin(), candidate.score_indices().end());
    for (const auto& [index, unused] : proposal.signal.scores) {
        (void)unused;
        changed_scores.insert(index);
    }
    std::vector<std::size_t> score_indices;
    std::vector<float> score_values;
    score_indices.reserve(changed_scores.size());
    score_values.reserve(changed_scores.size());
    for (const auto index : changed_scores) {
        if (index >= score_count) continue;
        score_indices.push_back(index);
        const auto found = proposal.signal.scores.find(index);
        score_values.push_back(found == proposal.signal.scores.end() ?
            candidate.score[index] : found->second);
    }
    std::vector<float> appended_scores;
    appended_scores.reserve(candidate.score.size() - score_count);
    for (std::size_t index = score_count; index < candidate.score.size(); ++index) {
        const auto found = proposal.signal.scores.find(index);
        appended_scores.push_back(found == proposal.signal.scores.end() ?
            candidate.score[index] : found->second);
    }
    const auto score_bytes = float32_bytes(score_values);
    const std::vector<std::size_t> score_shape{score_values.size()};
    const NumericArrayView score_view{"<f4", score_shape, score_bytes};
    const auto appended_score_bytes = float32_bytes(appended_scores);
    const std::vector<std::size_t> appended_score_shape{appended_scores.size()};
    const NumericArrayView appended_score_view{
        "<f4", appended_score_shape, appended_score_bytes};
    const auto stored_scores = VrsArrayBlocks::patch_and_append(
        scores, score_indices, &score_view, &appended_score_view, codec);

    const auto strength_count = inputs->strength.size();
    std::set<std::size_t> changed_strengths(
        candidate.strength_indices().begin(), candidate.strength_indices().end());
    for (const auto& [index, unused] : proposal.signal.strengths) {
        (void)unused;
        changed_strengths.insert(index);
    }
    std::vector<std::size_t> strength_indices;
    std::vector<float> strength_values;
    strength_indices.reserve(changed_strengths.size());
    strength_values.reserve(changed_strengths.size());
    for (const auto index : changed_strengths) {
        if (index >= strength_count) continue;
        strength_indices.push_back(index);
        const auto found = proposal.signal.strengths.find(index);
        strength_values.push_back(found == proposal.signal.strengths.end() ?
            candidate.strength[index] : found->second);
    }
    std::vector<float> appended_strengths;
    appended_strengths.reserve(candidate.strength.size() - strength_count);
    for (std::size_t index = strength_count; index < candidate.strength.size(); ++index) {
        const auto found = proposal.signal.strengths.find(index);
        appended_strengths.push_back(found == proposal.signal.strengths.end() ?
            candidate.strength[index] : found->second);
    }
    const auto strength_bytes = float16_bytes(strength_values);
    const std::vector<std::size_t> strength_shape{strength_values.size()};
    const NumericArrayView strength_view{"<f2", strength_shape, strength_bytes};
    const auto appended_strength_bytes = float16_bytes(appended_strengths);
    const std::vector<std::size_t> appended_strength_shape{appended_strengths.size()};
    const NumericArrayView appended_strength_view{
        "<f2", appended_strength_shape, appended_strength_bytes};
    const auto stored_strengths = VrsArrayBlocks::patch_and_append(
        strengths, strength_indices, &strength_view, &appended_strength_view, codec);

    std::vector<VrsExperiencePromotionDecision> promotions;
    if (proposal.signal.strength_receipt) {
        promotions.reserve(proposal.signal.strength_receipt->updates.size());
        for (const auto& update : proposal.signal.strength_receipt->updates) {
            const auto delimiter = update.connection_id.rfind(':');
            if (delimiter == std::string::npos) reject("invalid connection address");
            std::size_t edge{};
            const auto suffix = std::string_view(update.connection_id).substr(delimiter + 1);
            const auto parsed = std::from_chars(suffix.data(), suffix.data() + suffix.size(), edge);
            if (parsed.ec != std::errc{} || parsed.ptr != suffix.data() + suffix.size() ||
                edge >= candidate.strength.size())
                reject("invalid connection address");
            const auto found = proposal.signal.strengths.find(edge);
            const float value = found == proposal.signal.strengths.end() ?
                candidate.strength[edge] : found->second;
            promotions.push_back(assess_vrs_experience_promotion(
                candidate.snapshot_id, update.connection_id,
                update.previous_strength, static_cast<double>(value)));
        }
    }
    auto bound_proposal = std::make_shared<const StorageBoundEventSignalProposal>(proposal);
    return PreparedEventSignalStorage(
        shared_from_this(), std::move(bound_proposal), stored_scores.array,
        stored_strengths.array, std::move(promotions),
        {stored_scores.receipt, stored_strengths.receipt});
}

}  // namespace swegca::world
