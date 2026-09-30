#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view lossless_blocks_source_sha256 =
    "3d6ba40d5efb06cbad8ad751a04b3f33936a03d9950c7cb5090395e364dcaaf9";
inline constexpr std::string_view lossless_blocks_schema =
    "rozephine-lossless-resident-blocks-v1";
inline constexpr std::size_t maximum_lossless_block_bytes = 4U * 1024U * 1024U;
inline constexpr std::size_t default_lossless_block_bytes = 64U * 1024U;

class CorruptLosslessBlock final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

enum class LosslessBlockCodec : std::uint8_t { raw, zlib, zstd };

struct LosslessReadReceipt final {
    std::vector<std::byte> data;
    std::vector<std::size_t> block_indices;
    std::size_t decoded_bytes{};
};

class LosslessBlock final {
public:
    LosslessBlock(LosslessBlockCodec codec, std::vector<std::byte> payload,
                  std::size_t raw_size, std::uint32_t crc32);

    [[nodiscard]] std::vector<std::byte> decode() const;
    [[nodiscard]] std::string_view codec_name() const noexcept;

    const LosslessBlockCodec codec;
    const std::vector<std::byte> payload;
    const std::size_t raw_size;
    const std::uint32_t crc32;
};

class LosslessBlob final {
public:
    LosslessBlob(std::vector<std::shared_ptr<const LosslessBlock>> blocks,
                 std::size_t block_bytes, std::size_t raw_size,
                 std::string content_sha256);

    [[nodiscard]] static std::shared_ptr<const LosslessBlob> build(
        std::span<const std::byte> raw,
        LosslessBlockCodec codec = LosslessBlockCodec::zlib,
        int level = 3,
        std::size_t block_bytes = default_lossless_block_bytes);
    [[nodiscard]] LosslessReadReceipt read(
        std::size_t start = 0,
        std::optional<std::size_t> stop = std::nullopt) const;
    void verify_cold() const;
    [[nodiscard]] std::size_t stored_payload_bytes() const noexcept;
    [[nodiscard]] std::size_t resident_size_estimate() const noexcept;

    const std::vector<std::shared_ptr<const LosslessBlock>> blocks;
    const std::size_t block_bytes;
    const std::size_t raw_size;
    const std::string content_sha256;
};

struct ResidentRecordStats final {
    std::size_t address_count{};
    std::size_t unique_blob_count{};
    std::size_t logical_raw_bytes{};
    std::size_t unique_raw_bytes{};
    std::size_t stored_payload_bytes{};
    std::size_t address_table_estimate_bytes{};
    std::size_t resident_estimate_bytes{};
};

class ResidentRecordStore final {
public:
    using Record = std::pair<std::string, std::vector<std::byte>>;

    explicit ResidentRecordStore(
        std::map<std::string, std::shared_ptr<const LosslessBlob>, std::less<>> records);
    [[nodiscard]] static ResidentRecordStore build(
        std::vector<Record> records,
        LosslessBlockCodec codec = LosslessBlockCodec::zlib,
        int level = 3,
        std::size_t block_bytes = default_lossless_block_bytes);
    [[nodiscard]] LosslessReadReceipt read(
        std::string_view address, std::size_t start = 0,
        std::optional<std::size_t> stop = std::nullopt) const;
    [[nodiscard]] ResidentRecordStats stats() const noexcept;

    const std::map<std::string, std::shared_ptr<const LosslessBlob>, std::less<>> records;

private:
    std::size_t mapping_size_{};
};

}  // namespace swegca::world
