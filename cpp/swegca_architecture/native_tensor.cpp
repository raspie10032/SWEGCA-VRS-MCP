#include "swegca_architecture/native_tensor.hpp"

#include "swegca_architecture/resource_limits.hpp"

#include <limits>
#include <stdexcept>
#include <utility>

namespace swegca::architecture {
namespace {

// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:243-251
std::uint64_t checked_elements(const TensorShape3& shape) {
    if (shape.batches == 0 || shape.slots == 0 || shape.width == 0)
        throw std::invalid_argument("cognitive_tensor_shape_must_be_positive");
    if (shape.batches > std::numeric_limits<std::uint64_t>::max() / shape.slots)
        throw std::overflow_error("cognitive_tensor_shape_overflow");
    const auto rows = shape.batches * shape.slots;
    if (rows > std::numeric_limits<std::uint64_t>::max() / shape.width)
        throw std::overflow_error("cognitive_tensor_shape_overflow");
    return rows * shape.width;
}

// Validate the request before the allocator reserves and allocates its bytes.
// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:243-251
std::size_t checked_bytes(ScalarType type, const TensorShape3& shape) {
    const auto elements = checked_elements(shape);
    const auto width = scalar_width(type);
    if (elements > std::numeric_limits<std::size_t>::max() / width ||
        elements > ResourceLimits::max_resident_bytes / width)
        throw std::overflow_error("cognitive_tensor_byte_count_overflow");
    return static_cast<std::size_t>(elements) * width;
}

// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:243-251
std::uint64_t little_u64(std::span<const std::byte> bytes) noexcept {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < bytes.size(); ++index)
        value |= static_cast<std::uint64_t>(
                     std::to_integer<std::uint8_t>(bytes[index]))
                 << (index * 8);
    return value;
}

// Persistent numeric state rejects NaN and infinity for every supported scalar
// type without reproducing a framework's tensor runtime.
// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:243-251
bool scalar_is_finite(ScalarType type, std::span<const std::byte> bytes) noexcept {
    const auto bits = little_u64(bytes);
    switch (type) {
        case ScalarType::bfloat16:
            return (bits & 0x7f80u) != 0x7f80u;
        case ScalarType::float16:
            return (bits & 0x7c00u) != 0x7c00u;
        case ScalarType::float32:
            return (bits & 0x7f800000u) != 0x7f800000u;
        case ScalarType::float64:
            return (bits & 0x7ff0000000000000ull) != 0x7ff0000000000000ull;
    }
    return false;
}

// IEEE signed zero has one SWEGCA value identity. Canonical storage clears the
// sign bit so equal zero-valued deltas cannot acquire different digests.
// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:269-277
void normalize_signed_zero(std::span<std::byte> bytes) noexcept {
    const auto bits = little_u64(bytes);
    const auto sign = std::uint64_t{1} << (bytes.size() * 8 - 1);
    if ((bits & ~sign) == 0)
        bytes.back() &= std::byte{0x7f};
}

}  // namespace

// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:243-251
std::size_t scalar_width(ScalarType scalar_type) {
    switch (scalar_type) {
        case ScalarType::bfloat16:
        case ScalarType::float16:
            return 2;
        case ScalarType::float32:
            return 4;
        case ScalarType::float64:
            return 8;
    }
    throw std::invalid_argument("cognitive_tensor_scalar_type_invalid");
}

// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:243-251
CognitiveTensor::CognitiveTensor(const MemoryLedger::Account& account,
                                 ScalarType scalar_type, TensorShape3 shape,
                                 std::span<const std::byte> canonical_bytes)
    : CognitiveTensor(scalar_type, shape, [&] {
          if (canonical_bytes.size() != checked_bytes(scalar_type, shape))
              throw std::invalid_argument("cognitive_tensor_byte_count_mismatch");
          return Storage(canonical_bytes.begin(), canonical_bytes.end(),
                         account.allocator<std::byte>());
      }()) {}

// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:243-251
CognitiveTensor::CognitiveTensor(ScalarType scalar_type, TensorShape3 shape,
                                 Storage canonical_bytes)
    : scalar_type_(scalar_type), shape_(shape),
      canonical_bytes_(std::move(canonical_bytes)) {
    const auto elements = checked_elements(shape_);
    const auto width = scalar_width(scalar_type_);
    if (elements > std::numeric_limits<std::size_t>::max() / width ||
        canonical_bytes_.size() != static_cast<std::size_t>(elements) * width)
        throw std::invalid_argument("cognitive_tensor_byte_count_mismatch");
    if (canonical_bytes_.size() > ResourceLimits::max_resident_bytes)
        throw std::length_error("cognitive_tensor_exceeds_resident_limit");
    for (std::size_t offset = 0; offset < canonical_bytes_.size(); offset += width) {
        normalize_signed_zero(
            std::span<std::byte>(canonical_bytes_).subspan(offset, width));
        if (!scalar_is_finite(
                scalar_type_,
                std::span<const std::byte>(canonical_bytes_).subspan(offset, width)))
            throw std::invalid_argument("cognitive_tensor_value_not_finite");
    }
}

// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:243-251
CognitiveTensor CognitiveTensor::zeroed(const MemoryLedger::Account& account,
                                         ScalarType scalar_type,
                                         TensorShape3 shape) {
    const auto bytes = checked_bytes(scalar_type, shape);
    return CognitiveTensor(
        scalar_type, shape,
        Storage(bytes, std::byte{0}, account.allocator<std::byte>()));
}

// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:243-251
std::uint64_t CognitiveTensor::element_count() const noexcept {
    return shape_.batches * shape_.slots * shape_.width;
}

}  // namespace swegca::architecture
