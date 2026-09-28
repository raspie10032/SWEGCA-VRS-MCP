#pragma once

#include "checkpoint/checkpoint_profile.hpp"
#include "checkpoint/restricted_zip.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace swegca::checkpoint {

struct CheckpointTensor final {
    std::string state_path;
    std::string state_dict_key;
    std::string storage_key;
    std::string storage_member;
    TensorDType dtype{};
    std::vector<std::uint64_t> shape;
    std::vector<std::int64_t> stride;
    std::uint64_t storage_offset_elements{};
    std::uint64_t storage_elements{};
    std::uint64_t numel{};
    std::uint64_t logical_bytes{};
    std::uint64_t storage_span_start_byte{};
    std::uint64_t storage_span_end_byte_exclusive{};
    bool requires_grad{};
    bool model_state{};
    bool contiguous{};
};

struct CheckpointManifest final {
    const CheckpointProfile* profile{};
    std::string archive_root;
    std::string checkpoint_sha256;
    std::string data_pickle_sha256;
    std::string model_config_canonical_sha256;
    std::string serialization_id;
    std::size_t pickle_opcode_count{};
    std::vector<CheckpointTensor> tensors;

    [[nodiscard]] std::span<const CheckpointTensor> model_tensors() const noexcept;
    [[nodiscard]] const CheckpointTensor* find_model_tensor(std::string_view key) const noexcept;
};

// Exact, fail-closed reader for the three audited PyTorch ZIP checkpoints.
// open() publishes no manifest until file identity, ZIP integrity, symbolic
// pickle structure, config binding, tensor layout and storage records all pass.
class RestrictedCheckpoint final {
public:
    [[nodiscard]] static RestrictedCheckpoint open(const std::filesystem::path& path);

    RestrictedCheckpoint(const RestrictedCheckpoint&) = delete;
    RestrictedCheckpoint& operator=(const RestrictedCheckpoint&) = delete;
    RestrictedCheckpoint(RestrictedCheckpoint&&) noexcept = default;
    RestrictedCheckpoint& operator=(RestrictedCheckpoint&&) noexcept = default;

    [[nodiscard]] const CheckpointManifest& manifest() const noexcept { return manifest_; }
    [[nodiscard]] std::vector<std::byte> read_storage(std::string_view storage_key) const;

private:
    RestrictedCheckpoint(RestrictedZip archive, CheckpointManifest manifest)
        : archive_(std::move(archive)), manifest_(std::move(manifest)) {}

    RestrictedZip archive_;
    CheckpointManifest manifest_;
};

}  // namespace swegca::checkpoint
