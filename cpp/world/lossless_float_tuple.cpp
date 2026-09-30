#include "world/lossless_float_tuple.hpp"

#include "world/vrs_event_signal.hpp"

#include <bit>
#include <cstring>
#include <stdexcept>

namespace swegca::world {
namespace {

[[nodiscard]] std::size_t item_size(const ExactFloatFormat format) noexcept {
    if (format == ExactFloatFormat::binary16) return 2;
    if (format == ExactFloatFormat::binary32) return 4;
    return 8;
}

template<class Value>
[[nodiscard]] std::vector<std::byte> bytes(const std::vector<Value>& values) {
    static_assert(std::endian::native == std::endian::little);
    const auto span = std::as_bytes(std::span(values));
    return {span.begin(), span.end()};
}

[[nodiscard]] std::vector<double> restore(
    const std::span<const std::byte> payload, const ExactFloatFormat format) {
    const auto width = item_size(format);
    if (payload.size() % width) throw std::invalid_argument("invalid exact float tuple layout");
    std::vector<double> result;
    result.reserve(payload.size() / width);
    for (std::size_t offset = 0; offset != payload.size(); offset += width) {
        if (format == ExactFloatFormat::binary16) {
            const auto bits = static_cast<std::uint16_t>(std::to_integer<unsigned>(payload[offset])) |
                static_cast<std::uint16_t>(std::to_integer<unsigned>(payload[offset + 1]) << 8U);
            result.push_back(event_strength_from_float16_bits(bits));
        } else if (format == ExactFloatFormat::binary32) {
            std::uint32_t bits{};
            std::memcpy(&bits, payload.data() + offset, sizeof(bits));
            result.push_back(std::bit_cast<float>(bits));
        } else {
            std::uint64_t bits{};
            std::memcpy(&bits, payload.data() + offset, sizeof(bits));
            result.push_back(std::bit_cast<double>(bits));
        }
    }
    return result;
}

[[nodiscard]] bool exact(const std::span<const double> left,
                         const std::span<const double> right) noexcept {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index != left.size(); ++index)
        if (std::bit_cast<std::uint64_t>(left[index]) !=
            std::bit_cast<std::uint64_t>(right[index])) return false;
    return true;
}

}  // namespace

PackedFloatTuple::PackedFloatTuple(
    std::shared_ptr<const LosslessBlob> blob_value, const std::size_t count_value,
    const ExactFloatFormat format_value)
    : blob(std::move(blob_value)), count(count_value), format(format_value) {
    if (!blob || !count || blob->raw_size != count * item_size(format))
        throw std::invalid_argument("invalid exact float tuple layout");
}

PackedFloatTuple PackedFloatTuple::build(
    const std::span<const double> values, const LosslessBlockCodec codec,
    const int level, const std::size_t block_bytes) {
    if (values.empty())
        throw std::invalid_argument("immutable homogeneous float tuple required");

    std::vector<double> original(values.begin(), values.end());
    auto chosen = ExactFloatFormat::binary64;
    auto packed = bytes(original);

    std::vector<std::uint16_t> half;
    half.reserve(values.size());
    for (const auto value : values)
        half.push_back(event_strength_float16_bits(static_cast<float>(value)));
    auto candidate = bytes(half);
    if (exact(values, restore(candidate, ExactFloatFormat::binary16))) {
        chosen = ExactFloatFormat::binary16;
        packed = std::move(candidate);
    } else {
        std::vector<float> single;
        single.reserve(values.size());
        for (const auto value : values) single.push_back(static_cast<float>(value));
        candidate = bytes(single);
        if (exact(values, restore(candidate, ExactFloatFormat::binary32))) {
            chosen = ExactFloatFormat::binary32;
            packed = std::move(candidate);
        }
    }

    PackedFloatTuple result(
        LosslessBlob::build(packed, codec, level, block_bytes), values.size(), chosen);
    if (!exact(values, result.value()))
        throw std::invalid_argument("numeric bit-exact restoration failed");
    return result;
}

std::vector<double> PackedFloatTuple::value() const {
    return restore(blob->read().data, format);
}

std::string_view PackedFloatTuple::format_name() const noexcept {
    if (format == ExactFloatFormat::binary16) return "e";
    if (format == ExactFloatFormat::binary32) return "f";
    return "d";
}

}  // namespace swegca::world
