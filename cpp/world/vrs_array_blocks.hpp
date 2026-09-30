#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view vrs_array_blocks_source_sha256 =
    "52689577b7cbb1d4fc14cc5f4c84eb35a1cbfc3692a493622321176de2c40e24";
inline constexpr std::string_view vrs_block_store_source_sha256 =
    "d0ac58b872e73943e40d7ec4ecfdfb2f2b9f01c762ffc697800d7cce8edf2b19";
inline constexpr std::string_view vrs_array_bundle_schema =
    "rozephine-vrs-typed-array-bundle-v1";
inline constexpr std::string_view vrs_block_generation_schema =
    "rozephine-vrs-cow-blocks-v1";
inline constexpr std::size_t maximum_vrs_block_bytes = 4U * 1024U * 1024U;

class CorruptVrsBlock final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

enum class VrsBlockCodec : std::uint8_t { raw, zlib };

struct NumericArrayView final {
    std::string_view dtype;
    std::span<const std::size_t> shape;
    std::span<const std::byte> bytes;
    bool c_contiguous{true};
};

class LosslessVrsBlock final {
public:
    LosslessVrsBlock(VrsBlockCodec codec, std::vector<std::byte> payload,
                     std::size_t raw_size, std::uint32_t crc32);
    [[nodiscard]] std::vector<std::byte> decode() const;
    [[nodiscard]] std::string_view codec_name() const noexcept;

    const VrsBlockCodec codec;
    const std::vector<std::byte> payload;
    const std::size_t raw_size;
    const std::uint32_t crc32;
};

class StoredVrsBlock final {
public:
    StoredVrsBlock(std::shared_ptr<const LosslessVrsBlock> block, std::string digest);
    [[nodiscard]] static std::shared_ptr<const StoredVrsBlock>
    build(std::span<const std::byte> raw, VrsBlockCodec codec = VrsBlockCodec::zlib);

    const std::shared_ptr<const LosslessVrsBlock> block;
    const std::string digest;
};

class VrsBlockGeneration final {
public:
    VrsBlockGeneration(std::vector<std::shared_ptr<const StoredVrsBlock>> blocks,
                       std::size_t block_bytes, std::size_t raw_size);
    [[nodiscard]] static std::shared_ptr<const VrsBlockGeneration>
    build(std::span<const std::byte> raw, std::size_t block_bytes = 65536,
          VrsBlockCodec codec = VrsBlockCodec::zlib);
    [[nodiscard]] std::vector<std::byte> read(std::size_t start = 0,
                                              std::optional<std::size_t> stop = {}) const;

    const std::vector<std::shared_ptr<const StoredVrsBlock>> blocks;
    const std::size_t block_bytes;
    const std::size_t raw_size;
};

struct VrsBlockSaveReceipt final {
    std::string manifest_sha256;
    std::size_t block_publish_count{};
    std::size_t block_publish_bytes{};
    std::size_t manifest_bytes{};
    bool main_committed{false};
    bool current_pointer_written{false};
};

class VrsGenerationBlockStore final {
public:
    explicit VrsGenerationBlockStore(std::filesystem::path root);
    [[nodiscard]] VrsBlockSaveReceipt save(const VrsBlockGeneration& generation);
    [[nodiscard]] std::shared_ptr<const VrsBlockGeneration>
    load(std::string_view digest, std::size_t maximum_raw_bytes,
         std::size_t maximum_manifest_bytes = 16U * 1024U * 1024U);
    [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }
    void publish_immutable(std::string_view name, std::span<const std::byte> data);
    [[nodiscard]] std::vector<std::byte> read_immutable(
        std::string_view name, std::size_t maximum_bytes) const;

private:
    friend class VrsArrayBundle;
    void publish(std::string_view name, std::span<const std::byte> data);
    [[nodiscard]] std::vector<std::byte> read_limited(
        const std::filesystem::path& path, std::size_t maximum_bytes) const;

    std::filesystem::path root_;
    std::unordered_map<std::string, std::weak_ptr<const StoredVrsBlock>> verified_;
};

struct VrsArrayPrepareReceipt final {
    std::size_t candidate_bytes_scanned{};
    std::size_t candidate_bytes_compared{};
    std::size_t raw_bytes_encoded{};
    bool numerical_incrementality_claimed{false};
};

struct VrsArrayPatchReceipt final {
    std::size_t candidate_bytes_scanned{};
    std::size_t old_payload_bytes_decoded{};
    std::size_t raw_bytes_encoded{};
    bool numerical_incrementality_claimed{false};
    bool sparse_storage_update{true};
};

class VrsArrayBlocks final {
public:
    VrsArrayBlocks(std::string dtype, std::vector<std::size_t> shape,
                   std::shared_ptr<const VrsBlockGeneration> data);

    struct Prepared;
    [[nodiscard]] static Prepared prepare(
        NumericArrayView array, std::shared_ptr<const VrsArrayBlocks> parent = {},
        std::size_t block_bytes = 65536,
        VrsBlockCodec codec = VrsBlockCodec::zlib);

    struct Patched;
    [[nodiscard]] static Patched patch_and_append(
        const std::shared_ptr<const VrsArrayBlocks>& self,
        std::span<const std::size_t> indices = {},
        const NumericArrayView* values = nullptr,
        const NumericArrayView* append = nullptr,
        VrsBlockCodec codec = VrsBlockCodec::zlib);

    [[nodiscard]] std::vector<std::byte> restore() const;
    [[nodiscard]] std::size_t item_size() const;

    const std::string dtype;
    const std::vector<std::size_t> shape;
    const std::shared_ptr<const VrsBlockGeneration> data;
};

struct VrsArrayBlocks::Prepared final {
    std::shared_ptr<const VrsArrayBlocks> array;
    VrsArrayPrepareReceipt receipt;
};

struct VrsArrayBlocks::Patched final {
    std::shared_ptr<const VrsArrayBlocks> array;
    VrsArrayPatchReceipt receipt;
};

struct VrsArrayBundleSaveReceipt final {
    std::string bundle_sha256;
    std::map<std::string, VrsBlockSaveReceipt> arrays;
    std::size_t bundle_bytes{};
    bool main_committed{false};
};

struct VrsArrayBundlePrepared;

class VrsArrayBundle final {
public:
    explicit VrsArrayBundle(
        std::map<std::string, std::shared_ptr<const VrsArrayBlocks>> arrays);
    [[nodiscard]] static VrsArrayBundlePrepared prepare(
        const std::map<std::string, NumericArrayView>& arrays,
        const VrsArrayBundle* parent = nullptr,
        std::size_t block_bytes = 65536,
        VrsBlockCodec codec = VrsBlockCodec::zlib);
    [[nodiscard]] VrsArrayBundleSaveReceipt save(VrsGenerationBlockStore& store) const;
    [[nodiscard]] static VrsArrayBundle load(
        VrsGenerationBlockStore& store, std::string_view digest,
        std::size_t maximum_raw_bytes,
        std::size_t maximum_manifest_bytes = 16U * 1024U * 1024U);

    const std::map<std::string, std::shared_ptr<const VrsArrayBlocks>> arrays;
};

struct VrsArrayBundlePrepared final {
    VrsArrayBundle bundle;
    std::map<std::string, VrsArrayPrepareReceipt> receipts;
};

}  // namespace swegca::world
