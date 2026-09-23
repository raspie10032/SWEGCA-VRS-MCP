#pragma once

#include "swegca_architecture/allocation.hpp"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace swegca::architecture {

enum class ScalarType : std::uint8_t {
    bfloat16 = 1,
    float16 = 2,
    float32 = 3,
    float64 = 4,
};

enum class ByteOrder : std::uint8_t {
    little_endian = 1,
};

struct TensorShape3 final {
    std::uint64_t batches;
    std::uint64_t slots;
    std::uint64_t width;

    auto operator<=>(const TensorShape3&) const = default;
};

// Borrowed source for bounded initial-state recovery. A read may return less
// than requested; zero signals end or failure. The reader must allow an EOF
// probe at the exact expected byte count. CognitiveTensor requires every
// canonical byte, then requires EOF, and validates each complete chunk.
// Like any byte-reader interface, it trusts a reported read count to mean
// that many destination bytes were actually written.
class TensorByteReader {
public:
    virtual ~TensorByteReader() = default;
    [[nodiscard]] virtual std::size_t read(
        std::uint64_t offset, std::span<std::byte> destination) const = 0;
};

// Owned canonical storage for a fixed-rank [batch, slot, width] tensor. Bytes
// are always little-endian and expose no mutable view. Immutable bounded
// chunks can be shared by successive states; a verification-slot update
// copies only chunks intersecting that slot.
// Rule: native tensor value, reconstruction board@7c0b62f:243-251.
class CognitiveTensor final {
public:
    CognitiveTensor(const AllocationContext& account,
                    ScalarType scalar_type, TensorShape3 shape,
                    std::span<const std::byte> canonical_bytes);
    CognitiveTensor(const AllocationContext& account,
                    ScalarType scalar_type, TensorShape3 shape,
                    const TensorByteReader& source);

    [[nodiscard]] static CognitiveTensor zeroed(const AllocationContext& account,
                                                ScalarType scalar_type,
                                                TensorShape3 shape);

    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:243-251
    [[nodiscard]] ScalarType scalar_type() const noexcept { return scalar_type_; }
    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:243-251
    [[nodiscard]] ByteOrder byte_order() const noexcept {
        return ByteOrder::little_endian;
    }
    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:243-251
    [[nodiscard]] const TensorShape3& shape() const noexcept { return shape_; }
    [[nodiscard]] std::uint64_t element_count() const noexcept;
    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:243-251
    [[nodiscard]] std::uint64_t byte_count() const noexcept {
        return byte_count_;
    }

    // Visits the canonical byte stream in order without materializing a
    // second full tensor. Each span remains valid while this tensor lives.
    template <class Visit>
    void for_each_chunk(Visit&& visit) const {
        for (const auto& chunk : chunks_) visit(std::span<const std::byte>(chunk->bytes));
    }
    void copy_bytes(std::uint64_t offset, std::span<std::byte> destination) const;

    // Batch-one persistent-state operation. The value has one full slot's
    // canonical bytes; it is validated and normalized by the same rules as
    // construction. Other chunks are shared without mutation.
    [[nodiscard]] CognitiveTensor with_replaced_slot(
        const AllocationContext& account, std::uint64_t slot,
        std::span<const std::byte> value) const;

    [[nodiscard]] bool operator==(const CognitiveTensor& other) const noexcept;
    [[nodiscard]] std::strong_ordering operator<=>(const CognitiveTensor& other) const noexcept;

private:
    using Storage = std::vector<std::byte, AllocationAdapter<std::byte>>;
    struct Chunk final {
        Storage bytes;
        Chunk(Storage value, ScalarType type);
    };
    using ChunkPtr = std::shared_ptr<const Chunk>;
    using Chunks = std::vector<ChunkPtr, AllocationAdapter<ChunkPtr>>;
    static constexpr std::size_t chunk_bytes = 8u * 1024u * 1024u;
    CognitiveTensor(ScalarType scalar_type, TensorShape3 shape,
                    std::uint64_t byte_count, Chunks chunks) noexcept;

    ScalarType scalar_type_;
    TensorShape3 shape_;
    std::uint64_t byte_count_;
    Chunks chunks_;
};

[[nodiscard]] std::size_t scalar_width(ScalarType scalar_type);

}  // namespace swegca::architecture
