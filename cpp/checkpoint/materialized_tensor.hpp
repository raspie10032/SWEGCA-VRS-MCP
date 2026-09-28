#pragma once

#include "checkpoint/restricted_checkpoint.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace swegca::checkpoint {

class MaterializedTensor final {
public:
    MaterializedTensor(TensorDType dtype, std::vector<std::uint64_t> shape,
                       std::vector<std::byte> logical_bytes);

    [[nodiscard]] TensorDType dtype() const noexcept { return dtype_; }
    [[nodiscard]] std::span<const std::uint64_t> shape() const noexcept { return shape_; }
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return logical_bytes_; }
    [[nodiscard]] std::uint64_t numel() const noexcept { return numel_; }
    // Numeric inspection helper for floating tensors. bytes() remains the
    // exact bit interface; int64 must use int64_values().
    [[nodiscard]] double value_as_double(std::uint64_t logical_index) const;
    [[nodiscard]] std::vector<float> float32_values() const;
    [[nodiscard]] std::vector<std::int64_t> int64_values() const;

private:
    TensorDType dtype_;
    std::vector<std::uint64_t> shape_;
    std::vector<std::byte> logical_bytes_;
    std::uint64_t numel_{};
};

[[nodiscard]] MaterializedTensor materialize_tensor(
    const RestrictedCheckpoint& checkpoint, const CheckpointTensor& tensor);
[[nodiscard]] MaterializedTensor materialize_model_tensor(
    const RestrictedCheckpoint& checkpoint, std::string_view state_dict_key);

}  // namespace swegca::checkpoint
