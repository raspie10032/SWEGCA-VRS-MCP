#pragma once

#include "checkpoint/restricted_checkpoint.hpp"
#include "checkpoint/restricted_zip.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace swegca::checkpoint {

struct PrototypeMemberProfile final {
    std::string_view suffix;
    std::uint64_t size{};
    std::uint32_t crc32{};
    std::uint64_t payload_offset{};
};

struct PrototypeTensorProfile final {
    std::string_view key;
    std::string_view storage_key;
    TensorDType dtype{};
    std::span<const std::uint64_t> shape;
    std::span<const std::int64_t> stride;
    std::uint64_t storage_offset_elements{};
    std::uint64_t storage_elements{};
    bool requires_grad{};
};

struct PrototypeRuntimeCognitionConfig final {
    std::uint64_t semantic_slots{};
    std::uint64_t executive_slots{};
    std::uint64_t scratch_slots{};
    std::uint64_t hidden_dim{};
    std::uint64_t attention_heads{};
    std::uint64_t mlp_hidden_dim{};
    std::uint64_t minimum_cycles{};
    std::uint64_t maximum_cycles{};
    double halt_threshold{};
    double evidence_logit_epsilon{};
    double maximum_update{};
};

struct PrototypeCheckpointProfile final {
    std::string_view file_name;
    std::string_view original_path;
    std::string_view checkpoint_sha256;
    std::uint64_t checkpoint_bytes{};
    std::string_view data_pickle_sha256;
    std::uint64_t data_pickle_bytes{};
    std::size_t pickle_opcode_count{};
    std::string_view serialization_id;
    std::string_view tensor_manifest_sha256;
    std::string_view source_config_path;
    std::string_view source_config_sha256;
    std::string_view runtime_cognition_config_canonical_sha256;
    PrototypeRuntimeCognitionConfig runtime_cognition_config;
    std::span<const PrototypeTensorProfile> tensors;
    std::span<const PrototypeMemberProfile> members;
};

struct PrototypeCheckpointManifest final {
    const PrototypeCheckpointProfile* profile{};
    std::string archive_root;
    std::string checkpoint_sha256;
    std::string data_pickle_sha256;
    std::string serialization_id;
    std::string tensor_manifest_sha256;
    std::size_t pickle_opcode_count{};
    std::vector<CheckpointTensor> tensors;

    [[nodiscard]] const CheckpointTensor* find_tensor(std::string_view key) const noexcept;
};

// A separate, exact reader for the two audited Prototype0 bare state_dict
// artifacts. It deliberately does not widen RestrictedCheckpoint's accepted
// device, pickle opcode, root-shape, or checkpoint-identity contracts.
class PrototypeCheckpoint final {
public:
    [[nodiscard]] static PrototypeCheckpoint open(const std::filesystem::path& path);

    PrototypeCheckpoint(const PrototypeCheckpoint&) = delete;
    PrototypeCheckpoint& operator=(const PrototypeCheckpoint&) = delete;
    PrototypeCheckpoint(PrototypeCheckpoint&&) noexcept = default;
    PrototypeCheckpoint& operator=(PrototypeCheckpoint&&) noexcept = default;

    [[nodiscard]] const PrototypeCheckpointManifest& manifest() const noexcept { return manifest_; }
    [[nodiscard]] std::vector<std::byte> read_storage(std::string_view storage_key) const;

private:
    PrototypeCheckpoint(RestrictedZip archive, PrototypeCheckpointManifest manifest)
        : archive_(std::move(archive)), manifest_(std::move(manifest)) {}

    RestrictedZip archive_;
    PrototypeCheckpointManifest manifest_;
};

[[nodiscard]] std::span<const PrototypeCheckpointProfile>
prototype_checkpoint_profiles() noexcept;
[[nodiscard]] const PrototypeCheckpointProfile* find_prototype_checkpoint_profile(
    std::string_view checkpoint_sha256) noexcept;

}  // namespace swegca::checkpoint
