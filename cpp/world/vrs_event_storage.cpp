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
    std::shared_ptr<const BoundEventSignalStorage> binding_value)
    : signal(std::move(signal_value)), binding(std::move(binding_value)) {
    if (!binding || !signal.belongs_to(*binding->inputs))
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
    if (scores->restore() != float32_bytes(inputs->score))
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
    if (previous && previous->binding.get() != this)
        reject("event storage resume binding changed");
    const auto* prior_signal = previous ? &previous->signal : nullptr;
    auto result = settle_event_signal(
        *inputs, changed_nodes, std::move(strength_updates), connection_namespace,
        prior_signal, maximum_rounds, EventStrengthStorage::float16);
    return StorageBoundEventSignalProposal(
        std::move(result), shared_from_this());
}

PreparedEventSignalStorage BoundEventSignalStorage::prepare(
    const StorageBoundEventSignalProposal& proposal,
    const VrsBlockCodec codec) const {
    if (proposal.binding.get() != this || !proposal.signal.belongs_to(*inputs) ||
        proposal.signal.pending() ||
        proposal.signal.strength_storage != EventStrengthStorage::float16)
        reject("settled storage-bound event required");

    std::vector<std::size_t> score_indices;
    std::vector<float> score_values;
    score_indices.reserve(proposal.signal.scores.size());
    score_values.reserve(proposal.signal.scores.size());
    for (const auto& [index, value] : proposal.signal.scores) {
        score_indices.push_back(index);
        score_values.push_back(value);
    }
    const auto score_bytes = float32_bytes(score_values);
    const std::vector<std::size_t> score_shape{score_values.size()};
    const NumericArrayView score_view{"<f4", score_shape, score_bytes};
    const auto stored_scores = VrsArrayBlocks::patch_and_append(
        scores, score_indices, &score_view, nullptr, codec);

    std::vector<std::size_t> strength_indices;
    std::vector<float> strength_values;
    strength_indices.reserve(proposal.signal.strengths.size());
    strength_values.reserve(proposal.signal.strengths.size());
    for (const auto& [index, value] : proposal.signal.strengths) {
        strength_indices.push_back(index);
        strength_values.push_back(value);
    }
    const auto strength_bytes = float16_bytes(strength_values);
    const std::vector<std::size_t> strength_shape{strength_values.size()};
    const NumericArrayView strength_view{"<f2", strength_shape, strength_bytes};
    const auto stored_strengths = VrsArrayBlocks::patch_and_append(
        strengths, strength_indices, &strength_view, nullptr, codec);

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
                edge >= inputs->strength.size())
                reject("invalid connection address");
            const auto found = proposal.signal.strengths.find(edge);
            const float value = found == proposal.signal.strengths.end() ?
                inputs->strength[edge] : found->second;
            promotions.push_back(assess_vrs_experience_promotion(
                inputs->snapshot_id, update.connection_id,
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
