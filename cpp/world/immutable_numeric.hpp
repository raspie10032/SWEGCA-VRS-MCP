#pragma once

#include <cstddef>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view immutable_numeric_source_sha256 =
    "9b7b8c325d0859ddf90a85e77b682a174deb78fc114dfec964ff5c219a450027";

// Detached shape metadata over an immutable numeric owner. A mutable span is
// always copied. Only an already const shared owner may be reused.
template<class T>
class ImmutableNumericArray final {
    static_assert(std::is_trivially_copyable_v<T> && !std::is_pointer_v<T>);
public:
    ImmutableNumericArray(std::span<const T> values, std::vector<std::size_t> shape)
        : values_(std::make_shared<const std::vector<T>>(values.begin(), values.end())),
          shape_(std::move(shape)) { validate(); }

    ImmutableNumericArray(std::shared_ptr<const std::vector<T>> immutable_owner,
                          std::vector<std::size_t> shape)
        : values_(std::move(immutable_owner)), shape_(std::move(shape)) { validate(); }

    [[nodiscard]] std::span<const T> values() const noexcept { return *values_; }
    [[nodiscard]] std::span<const std::size_t> shape() const noexcept { return shape_; }
    [[nodiscard]] std::shared_ptr<const std::vector<T>> immutable_owner() const noexcept {
        return values_;
    }

private:
    void validate() {
        if (!values_ || shape_.empty())
            throw std::invalid_argument("plain non-object numeric array required");
        std::size_t elements = 1;
        for (const auto extent : shape_) {
            if (!extent || elements > std::numeric_limits<std::size_t>::max() / extent)
                throw std::invalid_argument("numeric array shape changed");
            elements *= extent;
        }
        if (elements != values_->size())
            throw std::invalid_argument("numeric array shape changed");
    }

    std::shared_ptr<const std::vector<T>> values_;
    std::vector<std::size_t> shape_;
};

}  // namespace swegca::world
