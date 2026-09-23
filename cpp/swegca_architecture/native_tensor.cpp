#include "swegca_architecture/native_tensor.hpp"

#include <algorithm>
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
    if (elements > std::numeric_limits<std::size_t>::max() / width)
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
// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:243-251
CognitiveTensor::Chunk::Chunk(Storage value, ScalarType type)
    : bytes(std::move(value)) {
    const auto width = scalar_width(type);
    if (bytes.empty() || bytes.size() % width != 0 || bytes.size() > chunk_bytes)
        throw std::invalid_argument("cognitive_tensor_chunk_size_invalid");
    for (std::size_t offset = 0; offset < bytes.size(); offset += width) {
        normalize_signed_zero(
            std::span<std::byte>(bytes).subspan(offset, width));
        if (!scalar_is_finite(
                type, std::span<const std::byte>(bytes).subspan(offset, width)))
            throw std::invalid_argument("cognitive_tensor_value_not_finite");
    }
}

// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:243-251
CognitiveTensor::CognitiveTensor(ScalarType scalar_type, TensorShape3 shape,
                                 std::uint64_t byte_count, Chunks chunks) noexcept
    : scalar_type_(scalar_type), shape_(shape), byte_count_(byte_count),
      chunks_(std::move(chunks)) {}

// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:243-251
CognitiveTensor::CognitiveTensor(const AllocationContext& account,
                                 ScalarType scalar_type, TensorShape3 shape,
                                 std::span<const std::byte> canonical_bytes)
    : scalar_type_(scalar_type), shape_(shape),
      byte_count_(checked_bytes(scalar_type, shape)),
      chunks_(account.allocator<ChunkPtr>()) {
    if (canonical_bytes.size() != byte_count_)
        throw std::invalid_argument("cognitive_tensor_byte_count_mismatch");
    chunks_.reserve(1 + (byte_count_ - 1) / chunk_bytes);
    for (std::size_t offset = 0; offset < byte_count_; offset += chunk_bytes) {
        const auto count = std::min<std::size_t>(chunk_bytes, byte_count_ - offset);
        Storage part(canonical_bytes.begin() + offset,
                     canonical_bytes.begin() + offset + count,
                     account.allocator<std::byte>());
        chunks_.push_back(std::allocate_shared<Chunk>(
            account.allocator<Chunk>(), std::move(part), scalar_type));
    }
}

// SWEGCA: docs/SWEGCA_CPP_MAIN_STATE_STORAGE_REVIEW.md@7c4d419:38-41
CognitiveTensor::CognitiveTensor(const AllocationContext& account,
                                 ScalarType scalar_type, TensorShape3 shape,
                                 const TensorByteReader& source)
    : scalar_type_(scalar_type), shape_(shape),
      byte_count_(checked_bytes(scalar_type, shape)),
      chunks_(account.allocator<ChunkPtr>()) {
    chunks_.reserve(1 + (byte_count_ - 1) / chunk_bytes);
    for (std::size_t offset = 0; offset < byte_count_; offset += chunk_bytes) {
        const auto count = std::min<std::size_t>(chunk_bytes, byte_count_ - offset);
        Storage part(count, std::byte{0}, account.allocator<std::byte>());
        std::size_t filled = 0;
        while (filled < count) {
            const auto remaining = std::span<std::byte>(part).subspan(filled);
            const auto received = source.read(offset + filled, remaining);
            if (received == 0 || received > remaining.size())
                throw std::invalid_argument("cognitive_tensor_source_short_read");
            filled += received;
        }
        chunks_.push_back(std::allocate_shared<Chunk>(
            account.allocator<Chunk>(), std::move(part), scalar_type));
    }
}

// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:243-251
CognitiveTensor CognitiveTensor::zeroed(const AllocationContext& account,
                                         ScalarType scalar_type,
                                         TensorShape3 shape) {
    const auto bytes = checked_bytes(scalar_type, shape);
    Chunks chunks(account.allocator<ChunkPtr>());
    chunks.reserve(1 + (bytes - 1) / chunk_bytes);
    const auto full_count = bytes / chunk_bytes;
    if (full_count != 0) {
        Storage zeros(chunk_bytes, std::byte{0}, account.allocator<std::byte>());
        auto shared = std::allocate_shared<Chunk>(
            account.allocator<Chunk>(), std::move(zeros), scalar_type);
        for (std::size_t index = 0; index < full_count; ++index)
            chunks.push_back(shared);
    }
    if (const auto rest = bytes % chunk_bytes; rest != 0) {
        Storage zeros(rest, std::byte{0}, account.allocator<std::byte>());
        chunks.push_back(std::allocate_shared<Chunk>(
            account.allocator<Chunk>(), std::move(zeros), scalar_type));
    }
    return CognitiveTensor(scalar_type, shape, bytes, std::move(chunks));
}

// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:243-251
std::uint64_t CognitiveTensor::element_count() const noexcept {
    return shape_.batches * shape_.slots * shape_.width;
}

// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:243-251
void CognitiveTensor::copy_bytes(std::uint64_t offset,
                                 std::span<std::byte> destination) const {
    if (offset > byte_count_ || destination.size() > byte_count_ - offset)
        throw std::out_of_range("cognitive_tensor_byte_range_invalid");
    std::size_t copied = 0;
    while (copied < destination.size()) {
        const auto at = static_cast<std::size_t>(offset + copied);
        const auto chunk_index = at / chunk_bytes;
        const auto within = at % chunk_bytes;
        const auto& source = chunks_[chunk_index]->bytes;
        const auto count = std::min(destination.size() - copied,
                                    source.size() - within);
        std::copy_n(source.begin() + within, count, destination.begin() + copied);
        copied += count;
    }
}

// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:243-251
CognitiveTensor CognitiveTensor::with_replaced_slot(
    const AllocationContext& account, std::uint64_t slot,
    std::span<const std::byte> value) const {
    if (shape_.batches != 1 || slot >= shape_.slots)
        throw std::invalid_argument("cognitive_tensor_slot_invalid");
    const auto slot_bytes = shape_.width * scalar_width(scalar_type_);
    if (value.size() != slot_bytes)
        throw std::invalid_argument("cognitive_tensor_slot_byte_count_mismatch");
    const auto begin = slot * slot_bytes;
    const auto end = begin + slot_bytes;
    Chunks result(chunks_.begin(), chunks_.end(), account.allocator<ChunkPtr>());
    for (auto at = begin; at < end;) {
        const auto chunk_index = static_cast<std::size_t>(at / chunk_bytes);
        const auto within = static_cast<std::size_t>(at % chunk_bytes);
        const auto count = std::min<std::uint64_t>(
            end - at, result[chunk_index]->bytes.size() - within);
        Storage changed(result[chunk_index]->bytes.begin(),
                        result[chunk_index]->bytes.end(),
                        account.allocator<std::byte>());
        std::copy_n(value.begin() + (at - begin), count,
                    changed.begin() + within);
        result[chunk_index] = std::allocate_shared<Chunk>(
            account.allocator<Chunk>(), std::move(changed), scalar_type_);
        at += count;
    }
    return CognitiveTensor(scalar_type_, shape_, byte_count_, std::move(result));
}

// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:243-251
bool CognitiveTensor::operator==(const CognitiveTensor& other) const noexcept {
    if (scalar_type_ != other.scalar_type_ || shape_ != other.shape_ ||
        byte_count_ != other.byte_count_) return false;
    for (std::size_t index = 0; index < chunks_.size(); ++index)
        if (chunks_[index] != other.chunks_[index] &&
            chunks_[index]->bytes != other.chunks_[index]->bytes) return false;
    return true;
}

// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:243-251
std::strong_ordering CognitiveTensor::operator<=>(const CognitiveTensor& other) const noexcept {
    if (const auto order = scalar_type_ <=> other.scalar_type_; order != 0)
        return order;
    if (const auto order = shape_ <=> other.shape_; order != 0)
        return order;
    for (std::size_t index = 0; index < chunks_.size(); ++index) {
        if (index >= other.chunks_.size()) return std::strong_ordering::greater;
        if (const auto order = chunks_[index]->bytes <=> other.chunks_[index]->bytes;
            order != 0) return order;
    }
    return chunks_.size() <=> other.chunks_.size();
}

}  // namespace swegca::architecture
