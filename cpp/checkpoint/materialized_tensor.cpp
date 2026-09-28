#include "checkpoint/materialized_tensor.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace swegca::checkpoint {
namespace {

[[nodiscard]] std::uint64_t checked_numel(const std::span<const std::uint64_t> shape) {
    if (std::find(shape.begin(), shape.end(), 0) != shape.end()) return 0;
    std::uint64_t count = 1;
    for (const auto dimension : shape) {
        if (dimension != 0 && count > std::numeric_limits<std::uint64_t>::max() / dimension) {
            throw std::overflow_error("materialized tensor element count overflow");
        }
        count *= dimension;
    }
    return count;
}

[[nodiscard]] std::uint16_t read_u16(const std::byte* bytes) noexcept {
    return static_cast<std::uint16_t>(std::to_integer<unsigned>(bytes[0])) |
           static_cast<std::uint16_t>(std::to_integer<unsigned>(bytes[1]) << 8U);
}

[[nodiscard]] std::uint32_t read_u32(const std::byte* bytes) noexcept {
    std::uint32_t value = 0;
    for (unsigned index = 0; index != 4; ++index) {
        value |= static_cast<std::uint32_t>(std::to_integer<unsigned>(bytes[index])) << (index * 8U);
    }
    return value;
}

[[nodiscard]] std::uint64_t read_u64(const std::byte* bytes) noexcept {
    std::uint64_t value = 0;
    for (unsigned index = 0; index != 8; ++index) {
        value |= static_cast<std::uint64_t>(std::to_integer<unsigned>(bytes[index])) << (index * 8U);
    }
    return value;
}

[[nodiscard]] double half_to_double(const std::uint16_t bits) noexcept {
    const bool negative = (bits & 0x8000U) != 0;
    const auto exponent = static_cast<unsigned>((bits >> 10U) & 0x1fU);
    const auto fraction = static_cast<unsigned>(bits & 0x03ffU);
    double value;
    if (exponent == 0x1fU) {
        value = fraction == 0 ? std::numeric_limits<double>::infinity()
                              : std::numeric_limits<double>::quiet_NaN();
    } else if (exponent == 0) {
        value = std::ldexp(static_cast<double>(fraction), -24);
    } else {
        value = std::ldexp(1.0 + static_cast<double>(fraction) / 1024.0,
                           static_cast<int>(exponent) - 15);
    }
    return std::copysign(value, negative ? -1.0 : 1.0);
}

[[nodiscard]] double bfloat16_to_double(const std::uint16_t bits) noexcept {
    return static_cast<double>(std::bit_cast<float>(static_cast<std::uint32_t>(bits) << 16U));
}

}  // namespace

MaterializedTensor::MaterializedTensor(const TensorDType dtype,
                                       std::vector<std::uint64_t> shape,
                                       std::vector<std::byte> logical_bytes)
    : dtype_(dtype), shape_(std::move(shape)), logical_bytes_(std::move(logical_bytes)),
      numel_(checked_numel(shape_)) {
    const auto item_size = dtype_size(dtype_);
    if (item_size == 0 || numel_ > std::numeric_limits<std::uint64_t>::max() / item_size ||
        logical_bytes_.size() != numel_ * item_size) {
        throw std::invalid_argument("materialized tensor byte count mismatch");
    }
}

double MaterializedTensor::value_as_double(const std::uint64_t logical_index) const {
    if (logical_index >= numel_) throw std::out_of_range("materialized tensor index out of range");
    const auto* bytes = logical_bytes_.data() + logical_index * dtype_size(dtype_);
    switch (dtype_) {
    case TensorDType::float16: return half_to_double(read_u16(bytes));
    case TensorDType::float32: return static_cast<double>(std::bit_cast<float>(read_u32(bytes)));
    case TensorDType::bfloat16: return bfloat16_to_double(read_u16(bytes));
    case TensorDType::int64:
        throw std::invalid_argument("int64 tensor requires exact int64_values access");
    }
    throw std::logic_error("unsupported materialized tensor dtype");
}

std::vector<float> MaterializedTensor::float32_values() const {
    if (dtype_ != TensorDType::float32) {
        throw std::invalid_argument("materialized tensor is not float32");
    }
    std::vector<float> values;
    values.reserve(static_cast<std::size_t>(numel_));
    for (std::uint64_t index = 0; index != numel_; ++index) {
        values.push_back(std::bit_cast<float>(read_u32(
            logical_bytes_.data() + index * sizeof(float))));
    }
    return values;
}

std::vector<std::int64_t> MaterializedTensor::int64_values() const {
    if (dtype_ != TensorDType::int64) {
        throw std::invalid_argument("materialized tensor is not int64");
    }
    std::vector<std::int64_t> values;
    values.reserve(static_cast<std::size_t>(numel_));
    for (std::uint64_t index = 0; index != numel_; ++index) {
        values.push_back(std::bit_cast<std::int64_t>(read_u64(
            logical_bytes_.data() + index * sizeof(std::int64_t))));
    }
    return values;
}

MaterializedTensor materialize_tensor(const RestrictedCheckpoint& checkpoint,
                                      const CheckpointTensor& tensor) {
    const auto bound = std::find_if(
        checkpoint.manifest().tensors.begin(), checkpoint.manifest().tensors.end(),
        [&](const CheckpointTensor& candidate) { return &candidate == &tensor; });
    if (bound == checkpoint.manifest().tensors.end()) {
        throw std::invalid_argument("checkpoint tensor descriptor is not bound to this checkpoint");
    }
    const TensorLayout layout{tensor.dtype, tensor.shape, tensor.stride,
                              tensor.storage_offset_elements, tensor.storage_elements};
    const auto checked = check_tensor_span(layout);
    if (!checked || checked.numel != tensor.numel || checked.logical_bytes != tensor.logical_bytes ||
        checked.storage_span_start_byte != tensor.storage_span_start_byte ||
        checked.storage_span_end_byte_exclusive != tensor.storage_span_end_byte_exclusive ||
        checked.contiguous != tensor.contiguous) {
        throw std::invalid_argument("checkpoint tensor descriptor is inconsistent");
    }
    const auto storage = checkpoint.read_storage(tensor.storage_key);
    const auto item_size = dtype_size(tensor.dtype);
    if (tensor.storage_elements > std::numeric_limits<std::uint64_t>::max() / item_size ||
        storage.size() != tensor.storage_elements * item_size) {
        throw std::runtime_error("checkpoint tensor storage size mismatch");
    }
    if (tensor.logical_bytes > std::numeric_limits<std::size_t>::max()) {
        throw std::length_error("materialized tensor exceeds addressable memory");
    }
    std::vector<std::byte> logical(static_cast<std::size_t>(tensor.logical_bytes));
    if (tensor.numel == 0) {
        return MaterializedTensor(tensor.dtype, tensor.shape, std::move(logical));
    }
    if (tensor.contiguous) {
        std::copy_n(storage.data() + tensor.storage_span_start_byte,
                    static_cast<std::size_t>(tensor.logical_bytes), logical.data());
        return MaterializedTensor(tensor.dtype, tensor.shape, std::move(logical));
    }
    std::vector<std::uint64_t> coordinates(tensor.shape.size(), 0);
    for (std::uint64_t logical_index = 0; logical_index != tensor.numel; ++logical_index) {
        std::uint64_t storage_index = tensor.storage_offset_elements;
        for (std::size_t dimension = 0; dimension != coordinates.size(); ++dimension) {
            const auto stride = static_cast<std::uint64_t>(tensor.stride[dimension]);
            if (coordinates[dimension] != 0 &&
                stride > (std::numeric_limits<std::uint64_t>::max() - storage_index) /
                             coordinates[dimension]) {
                throw std::overflow_error("checkpoint tensor logical offset overflow");
            }
            storage_index += coordinates[dimension] * stride;
        }
        if (storage_index >= tensor.storage_elements) {
            throw std::runtime_error("checkpoint tensor logical offset exceeds storage");
        }
        std::copy_n(storage.data() + storage_index * item_size, item_size,
                    logical.data() + logical_index * item_size);
        for (std::size_t reversed = coordinates.size(); reversed != 0; --reversed) {
            const auto dimension = reversed - 1;
            if (++coordinates[dimension] != tensor.shape[dimension]) break;
            coordinates[dimension] = 0;
        }
    }
    return MaterializedTensor(tensor.dtype, tensor.shape, std::move(logical));
}

MaterializedTensor materialize_model_tensor(const RestrictedCheckpoint& checkpoint,
                                            const std::string_view state_dict_key) {
    const auto* tensor = checkpoint.manifest().find_model_tensor(state_dict_key);
    if (tensor == nullptr) throw std::out_of_range("checkpoint model tensor is missing");
    return materialize_tensor(checkpoint, *tensor);
}

}  // namespace swegca::checkpoint
