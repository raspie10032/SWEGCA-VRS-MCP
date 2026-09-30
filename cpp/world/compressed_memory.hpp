#pragma once

#include "world/lossless_float_tuple.hpp"
#include "world/memory_activation.hpp"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view compressed_memory_source_sha256 =
    "6bf2102a4cdce72a5455d90f1f03825369a80f94d9123eb93407da07515644dc";

struct CompressionPolicy final {
    LosslessBlockCodec codec{LosslessBlockCodec::zlib};
    int level{3};
    std::size_t block_bytes{262144};
    std::size_t minimum_utf8_bytes{1024};
    void validate() const;
    friend bool operator==(const CompressionPolicy&, const CompressionPolicy&) = default;
};

struct CompressedText final {
    std::shared_ptr<const LosslessBlob> blob;
    [[nodiscard]] std::string value() const;
};

struct CompressedMemoryStats final {
    std::size_t compressed_string_references{};
    std::size_t unique_compressed_strings{};
    std::size_t logical_compressed_string_utf8_bytes{};
    std::size_t unique_compressed_string_utf8_bytes{};
    std::size_t compressed_string_payload_bytes{};
    std::size_t packed_numeric_tuple_references{};
    std::size_t packed_numeric_values{};
    std::size_t packed_numeric_payload_bytes{};
};

class CompressedMemoryActivationIndex final : public HotMemoryIndex {
public:
    [[nodiscard]] static std::shared_ptr<const CompressedMemoryActivationIndex> from_index(
        std::shared_ptr<const MemoryActivationIndex> source,
        CompressionPolicy policy);

    [[nodiscard]] std::string_view snapshot_id() const noexcept override;
    [[nodiscard]] std::size_t episode_count() const noexcept override;
    [[nodiscard]] const std::map<std::string, std::size_t, std::less<>>&
        outcome_counts() const noexcept override;
    [[nodiscard]] const MemoryEpisode& episode(std::string_view episode_id) const override;
    [[nodiscard]] bool contains_episode(std::string_view episode_id) const override;
    [[nodiscard]] std::vector<std::string> episode_ids_for_cue(
        std::string_view cue) const override;
    [[nodiscard]] std::vector<std::string> iter_episode_ids() const override;
    [[nodiscard]] std::vector<SemanticFamilyDirectory>
        semantic_family_directories() const override;

    const std::shared_ptr<const MemoryActivationIndex> index;
    const CompressionPolicy policy;
    const CompressedMemoryStats compression_stats;
    const std::map<std::string, std::shared_ptr<const CompressedText>, std::less<>>
        compressed_strings;
    const std::vector<std::shared_ptr<const PackedFloatTuple>> packed_numeric_tuples;

private:
    CompressedMemoryActivationIndex(
        std::shared_ptr<const MemoryActivationIndex> index,
        CompressionPolicy policy, CompressedMemoryStats stats,
        std::map<std::string, std::shared_ptr<const CompressedText>, std::less<>> strings,
        std::vector<std::shared_ptr<const PackedFloatTuple>> numeric);
};

[[nodiscard]] std::optional<CompressionPolicy> inherited_compression_policy(
    const std::shared_ptr<const HotMemoryIndex>& index);
[[nodiscard]] std::shared_ptr<const HotMemoryIndex> compress_hot_memory_index(
    std::shared_ptr<const HotMemoryIndex> index, CompressionPolicy policy);
[[nodiscard]] std::size_t resident_memory_bytes(
    const CompressedMemoryActivationIndex& index) noexcept;

}  // namespace swegca::world
