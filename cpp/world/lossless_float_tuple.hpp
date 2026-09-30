#pragma once

#include "world/lossless_blocks.hpp"

#include <cstddef>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view lossless_float_tuple_source_sha256 =
    "a6c7303ba1d6510d824055355896a6bfcf50e11c61183541c1304305a170a6e0";

enum class ExactFloatFormat : unsigned char { binary16, binary32, binary64 };

class PackedFloatTuple final {
public:
    PackedFloatTuple(std::shared_ptr<const LosslessBlob> blob,
                     std::size_t count, ExactFloatFormat format);
    [[nodiscard]] static PackedFloatTuple build(
        std::span<const double> values,
        LosslessBlockCodec codec = LosslessBlockCodec::zlib,
        int level = 3,
        std::size_t block_bytes = default_lossless_block_bytes);
    [[nodiscard]] std::vector<double> value() const;
    [[nodiscard]] std::string_view format_name() const noexcept;

    const std::shared_ptr<const LosslessBlob> blob;
    const std::size_t count;
    const ExactFloatFormat format;
};

}  // namespace swegca::world
