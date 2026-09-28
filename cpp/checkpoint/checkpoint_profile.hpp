#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace swegca::checkpoint {

enum class TensorDType : std::uint8_t {
    float16,
    float32,
    bfloat16,
    int64,
};

struct DTypeProfile final {
    TensorDType dtype{};
    std::uint64_t tensor_count{};
    std::uint64_t element_count{};
};

struct TensorLayout final {
    TensorDType dtype{};
    std::span<const std::uint64_t> shape{};
    std::span<const std::int64_t> stride{};
    std::uint64_t storage_offset_elements{};
    std::uint64_t storage_elements{};
};

enum class TensorLayoutError : std::uint8_t {
    none,
    unsupported_dtype,
    rank_mismatch,
    negative_stride,
    arithmetic_overflow,
    storage_out_of_bounds,
};

struct CheckedTensorSpan final {
    TensorLayoutError error{TensorLayoutError::none};
    std::uint64_t numel{};
    std::uint64_t logical_bytes{};
    std::uint64_t storage_span_start_byte{};
    std::uint64_t storage_span_end_byte_exclusive{};
    std::uint64_t storage_span_bytes{};
    bool contiguous{};

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return error == TensorLayoutError::none;
    }
};

struct CheckpointProfile final {
    std::string_view file_name;
    std::string_view original_path;
    std::string_view checkpoint_sha256;
    std::uint64_t checkpoint_bytes{};
    std::string_view data_pickle_sha256;
    std::string_view serialization_id;
    std::string_view model_config_canonical_sha256;
    std::string_view tensor_manifest_sha256;
    std::span<const std::string_view> model_state_keys;
    // Counts include every serialized tensor, including the non-model teacher
    // projection below. model_numel and model_state_keys cover only `model`.
    std::span<const DTypeProfile> dtype_profile;
    std::uint64_t model_tensor_count{};
    std::uint64_t all_tensor_count{};
    std::uint64_t model_numel{};
    std::uint64_t storage_record_count{};
    TensorLayout fixed_teacher_projection;
};

[[nodiscard]] std::uint64_t dtype_size(TensorDType dtype) noexcept;
[[nodiscard]] CheckedTensorSpan check_tensor_span(const TensorLayout& layout) noexcept;

// Ordered from the narrative checkpoint through cognition to synapse.  The
// later model key sets are strict append-only supersets in the audited files.
[[nodiscard]] std::span<const CheckpointProfile> checkpoint_profiles() noexcept;
[[nodiscard]] const CheckpointProfile* find_checkpoint_profile(
    std::string_view checkpoint_sha256) noexcept;
[[nodiscard]] bool contains_model_key(const CheckpointProfile& profile,
                                      std::string_view key) noexcept;
[[nodiscard]] bool validate_checkpoint_profile(const CheckpointProfile& profile) noexcept;

}  // namespace swegca::checkpoint
