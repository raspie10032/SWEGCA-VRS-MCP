#pragma once

#include <cstddef>
#include <memory>
#include <numeric>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace swegca::vrs {

// The owner is the proof of immutability. A borrowed span or read-only mmap is
// never accepted as an immutable owner without copying its bytes first.
template <typename T>
struct FrozenNumeric {
    static_assert(std::is_trivially_copyable_v<T>);
    std::shared_ptr<const std::vector<T>> owner;
    std::vector<std::size_t> shape;

    // SWEGCA: src/tinylm_slicer/mosaic_immutable_numeric.py@3bddcb7:8-21
    [[nodiscard]] std::span<const T> values() const {
        if (!owner) throw std::runtime_error("immutable_numeric_owner_missing");
        return *owner;
    }
};

// SWEGCA: src/tinylm_slicer/mosaic_immutable_numeric.py@3bddcb7:14-21
template <typename T>
[[nodiscard]] FrozenNumeric<T> freeze_numeric(std::span<const T> values,
                                               std::vector<std::size_t> shape) {
    static_assert(std::is_trivially_copyable_v<T>);
    std::size_t elements = 1;
    for (auto extent : shape) {
        if (extent && elements > static_cast<std::size_t>(-1) / extent)
            throw std::runtime_error("immutable_numeric_shape_overflow");
        elements *= extent;
    }
    if (elements != values.size()) throw std::runtime_error("immutable_numeric_shape_invalid");
    auto owner = std::make_shared<const std::vector<T>>(values.begin(), values.end());
    return FrozenNumeric<T>{std::move(owner), std::move(shape)};
}

// SWEGCA: src/tinylm_slicer/mosaic_immutable_numeric.py@3bddcb7:16-21
template <typename T>
[[nodiscard]] FrozenNumeric<T> reshape_frozen(const FrozenNumeric<T>& value,
                                               std::vector<std::size_t> shape) {
    std::size_t elements = 1;
    for (auto extent : shape) {
        if (extent && elements > static_cast<std::size_t>(-1) / extent)
            throw std::runtime_error("immutable_numeric_shape_overflow");
        elements *= extent;
    }
    if (!value.owner || elements != value.owner->size())
        throw std::runtime_error("immutable_numeric_shape_invalid");
    return FrozenNumeric<T>{value.owner, std::move(shape)};
}

}  // namespace swegca::vrs
