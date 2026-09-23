#pragma once

#include "swegca_architecture/memory_ledger.hpp"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
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

// Owned canonical storage for a fixed-rank [batch, slot, width] tensor. Bytes
// are always little-endian and expose no mutable view.
// Rule: native tensor value, reconstruction board@7c0b62f:243-251.
class CognitiveTensor final {
public:
    CognitiveTensor(const MemoryLedger::Account& account,
                    ScalarType scalar_type, TensorShape3 shape,
                    std::span<const std::byte> canonical_bytes);

    [[nodiscard]] static CognitiveTensor zeroed(const MemoryLedger::Account& account,
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
        return canonical_bytes_.size();
    }
    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:243-251
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
        return canonical_bytes_;
    }

    auto operator<=>(const CognitiveTensor&) const = default;

private:
    using Storage = std::vector<std::byte, MemoryLedger::Allocator<std::byte>>;
    CognitiveTensor(ScalarType scalar_type, TensorShape3 shape, Storage bytes);

    ScalarType scalar_type_;
    TensorShape3 shape_;
    Storage canonical_bytes_;
};

[[nodiscard]] std::size_t scalar_width(ScalarType scalar_type);

}  // namespace swegca::architecture
