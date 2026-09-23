#include "swegca_architecture/native_tensor.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <utility>

namespace swegca::architecture {
namespace {

// SWEGCA: src/swegca/mosaic_cognitive_kernel.py@5901a5a:175-254
std::uint64_t checked_elements(const TensorShape3& shape) {
    // The source state constructor permits an empty batch. Its configured
    // slot count and hidden width remain positive.
    if (shape.slots == 0 || shape.width == 0)
        throw std::invalid_argument("cognitive_tensor_slot_or_width_zero");
    if (shape.batches > std::numeric_limits<std::uint64_t>::max() / shape.slots)
        throw std::overflow_error("cognitive_tensor_shape_overflow");
    const auto rows = shape.batches * shape.slots;
    if (rows > std::numeric_limits<std::uint64_t>::max() / shape.width)
        throw std::overflow_error("cognitive_tensor_shape_overflow");
    return rows * shape.width;
}

// Validate the request before the allocator reserves and allocates its bytes.
// SWEGCA: src/swegca/mosaic_cognitive_kernel.py@5901a5a:175-254
std::size_t checked_bytes(ScalarType type, const TensorShape3& shape) {
    const auto elements = checked_elements(shape);
    const auto width = scalar_width(type);
    if (elements > std::numeric_limits<std::size_t>::max() / width)
        throw std::overflow_error("cognitive_tensor_byte_count_overflow");
    return static_cast<std::size_t>(elements) * width;
}

}  // namespace

// SWEGCA: src/swegca/mosaic_cognitive_kernel.py@5901a5a:175-254
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

// The source CognitiveState checks tensor type and shape, while the source
// SynapseProposal checks finite delta values separately. Storage must not
// erase signed zero or reject state values on the proposal's behalf.
// SWEGCA: src/swegca/mosaic_cognitive_kernel.py@5901a5a:220-254
// SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:65-89
CognitiveTensor::Chunk::Chunk(Storage value, ScalarType type)
    : bytes(std::move(value)) {
    const auto width = scalar_width(type);
    if (bytes.empty() || bytes.size() % width != 0 || bytes.size() > chunk_bytes)
        throw std::invalid_argument("cognitive_tensor_chunk_size_invalid");
}

// SWEGCA: src/swegca/mosaic_cognitive_kernel.py@5901a5a:175-254
CognitiveTensor::CognitiveTensor(ScalarType scalar_type, TensorShape3 shape,
                                 std::uint64_t byte_count, Chunks chunks) noexcept
    : scalar_type_(scalar_type), shape_(shape), byte_count_(byte_count),
      chunks_(std::move(chunks)) {}

// SWEGCA: src/swegca/mosaic_cognitive_kernel.py@5901a5a:175-254
CognitiveTensor::CognitiveTensor(const AllocationContext& account,
                                 ScalarType scalar_type, TensorShape3 shape,
                                 std::span<const std::byte> canonical_bytes)
    : scalar_type_(scalar_type), shape_(shape),
      byte_count_(checked_bytes(scalar_type, shape)),
      chunks_(account.allocator<ChunkPtr>()) {
    if (canonical_bytes.size() != byte_count_)
        throw std::invalid_argument("cognitive_tensor_byte_count_mismatch");
    if (byte_count_ != 0) chunks_.reserve(1 + (byte_count_ - 1) / chunk_bytes);
    for (std::size_t offset = 0; offset < byte_count_; offset += chunk_bytes) {
        const auto count = std::min<std::size_t>(chunk_bytes, byte_count_ - offset);
        Storage part(canonical_bytes.begin() + offset,
                     canonical_bytes.begin() + offset + count,
                     account.allocator<std::byte>());
        chunks_.push_back(std::allocate_shared<Chunk>(
            account.allocator<Chunk>(), std::move(part), scalar_type));
    }
}

// The user's CognitiveState source defines the tensor shape and value
// contract. Borrowed, bounded byte reading is a C++ recovery extension.
// SWEGCA: src/swegca/mosaic_cognitive_kernel.py@5901a5a:220-254
CognitiveTensor::CognitiveTensor(const AllocationContext& account,
                                 ScalarType scalar_type, TensorShape3 shape,
                                 const TensorByteReader& source)
    : scalar_type_(scalar_type), shape_(shape),
      byte_count_(checked_bytes(scalar_type, shape)),
      chunks_(account.allocator<ChunkPtr>()) {
    if (byte_count_ != 0) chunks_.reserve(1 + (byte_count_ - 1) / chunk_bytes);
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
    std::array<std::byte, 1> trailing{};
    if (source.read(byte_count_, trailing) != 0)
        throw std::invalid_argument("cognitive_tensor_byte_count_mismatch");
}

// SWEGCA: src/swegca/mosaic_cognitive_kernel.py@5901a5a:175-254
CognitiveTensor CognitiveTensor::zeroed(const AllocationContext& account,
                                         ScalarType scalar_type,
                                         TensorShape3 shape) {
    const auto bytes = checked_bytes(scalar_type, shape);
    Chunks chunks(account.allocator<ChunkPtr>());
    if (bytes != 0) chunks.reserve(1 + (bytes - 1) / chunk_bytes);
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

// SWEGCA: src/swegca/mosaic_cognitive_kernel.py@5901a5a:175-254
std::uint64_t CognitiveTensor::element_count() const noexcept {
    return shape_.batches * shape_.slots * shape_.width;
}

// SWEGCA: src/swegca/mosaic_cognitive_kernel.py@5901a5a:175-254
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

// The source's bounded write requires batch one before replacing a slot.
// SWEGCA: src/swegca/mosaic_bounded_world_write.py@5901a5a:341-342
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

// This compares the exact stored state representation. It is C++ storage
// identity, not the author's tensor elementwise numeric equality: +0 and -0
// compare differently here because the retained bits also enter state hashes.
// SWEGCA: src/swegca/mosaic_cognitive_kernel.py@5901a5a:175-254
bool CognitiveTensor::operator==(const CognitiveTensor& other) const noexcept {
    if (scalar_type_ != other.scalar_type_ || shape_ != other.shape_ ||
        byte_count_ != other.byte_count_) return false;
    for (std::size_t index = 0; index < chunks_.size(); ++index)
        if (chunks_[index] != other.chunks_[index] &&
            chunks_[index]->bytes != other.chunks_[index]->bytes) return false;
    return true;
}

// Lexicographic ordering is additional C++ storage infrastructure. The
// original CognitiveState has no tensor ordering contract.
// SWEGCA: src/swegca/mosaic_cognitive_kernel.py@5901a5a:175-254
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
