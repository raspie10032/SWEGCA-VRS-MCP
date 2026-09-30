#include "world/compressed_memory.hpp"

#include "swegca_architecture/sha256.hpp"
#include "world/vrs_generation_rebind.hpp"

#include <algorithm>
#include <span>
#include <stdexcept>

namespace swegca::world {
namespace {

std::string hex(const architecture::DigestBytes& digest) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result(digest.size() * 2, '0');
    for (std::size_t index = 0; index < digest.size(); ++index) {
        const auto value = std::to_integer<unsigned>(digest[index]);
        result[2 * index] = digits[value >> 4U];
        result[2 * index + 1] = digits[value & 15U];
    }
    return result;
}

struct Encoder final {
    explicit Encoder(const CompressionPolicy& value) : policy(value) {}
    const CompressionPolicy& policy;
    CompressedMemoryStats stats;
    std::map<std::string, std::shared_ptr<const CompressedText>, std::less<>> strings;
    std::vector<std::shared_ptr<const PackedFloatTuple>> numeric;

    void observe(const JsonValue& value) {
        if (const auto* text = std::get_if<std::string>(&value.storage())) {
            if (text->size() < policy.minimum_utf8_bytes) return;
            const auto bytes = std::as_bytes(std::span(text->data(), text->size()));
            const auto key = hex(architecture::Sha256::of(bytes));
            auto found = strings.find(key);
            if (found == strings.end()) {
                auto blob = LosslessBlob::build(bytes, policy.codec, policy.level, policy.block_bytes);
                if (blob->resident_size_estimate() >= text->size()) return;
                found = strings.emplace(key,
                    std::make_shared<const CompressedText>(CompressedText{std::move(blob)})).first;
                ++stats.unique_compressed_strings;
                stats.unique_compressed_string_utf8_bytes += text->size();
                stats.compressed_string_payload_bytes += found->second->blob->stored_payload_bytes();
            } else if (found->second->value() != *text) {
                throw std::invalid_argument("string content digest collision");
            }
            ++stats.compressed_string_references;
            stats.logical_compressed_string_utf8_bytes += text->size();
            return;
        }
        if (value.is_object()) {
            for (const auto& [unused, child] : value.as_object()) {
                static_cast<void>(unused); observe(child);
            }
            return;
        }
        if (!value.is_array()) return;
        const auto& array = value.as_array();
        if (array.size() >= 64 && std::ranges::all_of(array, [](const auto& item) {
                return std::holds_alternative<double>(item.storage());
            })) {
            std::vector<double> values;
            values.reserve(array.size());
            for (const auto& item : array) values.push_back(std::get<double>(item.storage()));
            auto packed = std::make_shared<const PackedFloatTuple>(PackedFloatTuple::build(
                values, policy.codec, policy.level, policy.block_bytes));
            if (packed->blob->resident_size_estimate() < values.size() * sizeof(double)) {
                ++stats.packed_numeric_tuple_references;
                stats.packed_numeric_values += values.size();
                stats.packed_numeric_payload_bytes += packed->blob->stored_payload_bytes();
                numeric.push_back(std::move(packed));
                return;
            }
        }
        for (const auto& child : array) observe(child);
    }
};

}  // namespace

void CompressionPolicy::validate() const {
    if (minimum_utf8_bytes < 1) throw std::invalid_argument("minimum_utf8_bytes must be positive");
    const std::span<const std::byte> empty;
    (void)LosslessBlob::build(empty, codec, level, block_bytes);
}

std::string CompressedText::value() const {
    if (!blob) throw std::invalid_argument("compressed text blob required");
    const auto data = blob->read().data;
    return {reinterpret_cast<const char*>(data.data()), data.size()};
}

CompressedMemoryActivationIndex::CompressedMemoryActivationIndex(
    std::shared_ptr<const MemoryActivationIndex> index_value,
    CompressionPolicy policy_value, CompressedMemoryStats stats,
    std::map<std::string, std::shared_ptr<const CompressedText>, std::less<>> strings,
    std::vector<std::shared_ptr<const PackedFloatTuple>> numeric)
    : index(std::move(index_value)), policy(policy_value), compression_stats(stats),
      compressed_strings(std::move(strings)), packed_numeric_tuples(std::move(numeric)) {
    if (!index || index->lookup_requires_io())
        throw std::invalid_argument("compressed index must wrap a validated hot source");
}

std::shared_ptr<const CompressedMemoryActivationIndex>
CompressedMemoryActivationIndex::from_index(
    std::shared_ptr<const MemoryActivationIndex> source, CompressionPolicy policy) {
    if (!source) throw std::invalid_argument("convert explicit materialized sources only");
    policy.validate();
    Encoder encoder(policy);
    for (const auto& [unused, episode] : source->episodes_by_id) {
        static_cast<void>(unused);
        for (const auto& step : episode.steps) encoder.observe(JsonValue(step.observation));
    }
    return std::shared_ptr<const CompressedMemoryActivationIndex>(
        new CompressedMemoryActivationIndex(std::move(source), policy, encoder.stats,
            std::move(encoder.strings), std::move(encoder.numeric)));
}

std::string_view CompressedMemoryActivationIndex::snapshot_id() const noexcept {
    return index->snapshot_id();
}
std::size_t CompressedMemoryActivationIndex::episode_count() const noexcept {
    return index->episode_count();
}
const std::map<std::string, std::size_t, std::less<>>&
CompressedMemoryActivationIndex::outcome_counts() const noexcept {
    return index->outcome_counts();
}
const MemoryEpisode& CompressedMemoryActivationIndex::episode(const std::string_view id) const {
    return index->episode(id);
}
std::vector<std::string> CompressedMemoryActivationIndex::episode_ids_for_cue(
    const std::string_view cue) const { return index->episode_ids_for_cue(cue); }
std::vector<std::string> CompressedMemoryActivationIndex::iter_episode_ids() const {
    return index->iter_episode_ids();
}
std::vector<SemanticFamilyDirectory>
CompressedMemoryActivationIndex::semantic_family_directories() const {
    return index->semantic_family_directories();
}

std::optional<CompressionPolicy> inherited_compression_policy(
    const std::shared_ptr<const HotMemoryIndex>& index) {
    if (!index) return std::nullopt;
    if (const auto compressed =
            std::dynamic_pointer_cast<const CompressedMemoryActivationIndex>(index))
        return compressed->policy;
    if (const auto composite = std::dynamic_pointer_cast<const CompositeMemoryActivationIndex>(index))
        for (auto at = composite->sources.rbegin(); at != composite->sources.rend(); ++at)
            if (auto found = inherited_compression_policy(*at)) return found;
    if (const auto bound = std::dynamic_pointer_cast<const VrsGenerationBoundMemoryIndex>(index))
        for (auto at = bound->ordinary_sources.rbegin(); at != bound->ordinary_sources.rend(); ++at)
            if (auto found = inherited_compression_policy(*at)) return found;
    return std::nullopt;
}

std::shared_ptr<const HotMemoryIndex> compress_hot_memory_index(
    std::shared_ptr<const HotMemoryIndex> index, CompressionPolicy policy) {
    if (!index) throw std::invalid_argument("hot memory source required");
    policy.validate();
    if (const auto compressed =
            std::dynamic_pointer_cast<const CompressedMemoryActivationIndex>(index)) {
        if (compressed->policy == policy) return index;
        return CompressedMemoryActivationIndex::from_index(compressed->index, policy);
    }
    if (const auto materialized = std::dynamic_pointer_cast<const MemoryActivationIndex>(index))
        return CompressedMemoryActivationIndex::from_index(materialized, policy);
    if (const auto composite = std::dynamic_pointer_cast<const CompositeMemoryActivationIndex>(index)) {
        std::vector<std::shared_ptr<const HotMemoryIndex>> sources;
        for (const auto& source : composite->sources)
            sources.push_back(compress_hot_memory_index(source, policy));
        auto converted = std::make_shared<const CompositeMemoryActivationIndex>(std::move(sources));
        if (converted->snapshot_id() != index->snapshot_id())
            throw std::invalid_argument("physical compression changed composite semantic identity");
        return converted;
    }
    if (const auto bound = std::dynamic_pointer_cast<const VrsGenerationBoundMemoryIndex>(index)) {
        std::vector<std::shared_ptr<const HotMemoryIndex>> ordinary;
        for (const auto& source : bound->ordinary_sources)
            ordinary.push_back(compress_hot_memory_index(source, policy));
        auto converted = std::make_shared<const VrsGenerationBoundMemoryIndex>(
            std::move(ordinary), bound->vrs_source, bound->effective_vrs_snapshot_id,
            bound->replaced_vrs_source_snapshot_ids);
        if (converted->snapshot_id() != index->snapshot_id())
            throw std::invalid_argument("physical compression changed generation-bound identity");
        return converted;
    }
    return index;
}

std::size_t resident_memory_bytes(const CompressedMemoryActivationIndex& value) noexcept {
    std::size_t result = sizeof(value);
    for (const auto& [key, text] : value.compressed_strings)
        result += key.capacity() + sizeof(*text) + text->blob->resident_size_estimate();
    for (const auto& numeric : value.packed_numeric_tuples)
        result += sizeof(*numeric) + numeric->blob->resident_size_estimate();
    return result;
}

}  // namespace swegca::world
