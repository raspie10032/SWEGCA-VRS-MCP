#include "swegca_architecture/scalar_codec.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace swegca::architecture {
namespace {

static_assert(sizeof(float) == 4 && sizeof(double) == 8);
static_assert(std::numeric_limits<float>::is_iec559 &&
              std::numeric_limits<double>::is_iec559);
static_assert(std::numeric_limits<float>::radix == 2 &&
              std::numeric_limits<double>::radix == 2);

// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:269-277
std::uint64_t read_le(std::span<const std::byte> bytes) noexcept {
    std::uint64_t bits = 0;
    for (std::size_t at = 0; at < bytes.size(); ++at)
        bits |= std::uint64_t(std::to_integer<std::uint8_t>(bytes[at])) << (8 * at);
    return bits;
}

// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:269-277
void write_le(std::uint64_t bits, std::span<std::byte> bytes) noexcept {
    for (std::size_t at = 0; at < bytes.size(); ++at)
        bytes[at] = static_cast<std::byte>((bits >> (8 * at)) & 0xff);
}

// Every finite IEEE binary16 value is exactly representable as binary32.
// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:269-277
float half_to_float(std::uint16_t bits) noexcept {
    const std::uint32_t sign = std::uint32_t(bits & 0x8000u) << 16;
    const std::uint32_t exponent = (bits >> 10) & 0x1fu;
    std::uint32_t fraction = bits & 0x3ffu;
    // A direct codec caller must not turn a half infinity or NaN into a
    // finite arbitration value. CognitiveTensor also rejects these bytes.
    if (exponent == 31) return std::bit_cast<float>(
        sign | (0xffu << 23) | (fraction << 13));
    if (exponent == 0 && fraction == 0) return std::bit_cast<float>(sign);
    if (exponent == 0) {
        int power = -14;
        while ((fraction & 0x400u) == 0) {
            fraction <<= 1;
            --power;
        }
        fraction &= 0x3ffu;
        return std::bit_cast<float>(sign | (std::uint32_t(power + 127) << 23) |
                                    (fraction << 13));
    }
    return std::bit_cast<float>(sign | ((exponent + 112u) << 23) |
                                (fraction << 13));
}

// Integer round-to-nearest-even; shift is 1..24 on the binary16 path.
// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:269-277
std::uint32_t round_shift_even(std::uint32_t value, unsigned shift) noexcept {
    const auto kept = value >> shift;
    const auto remainder = value & ((std::uint32_t{1} << shift) - 1);
    const auto halfway = std::uint32_t{1} << (shift - 1);
    return kept + (remainder > halfway || (remainder == halfway && (kept & 1u)));
}

// Finite binary32 -> binary16, with one ties-to-even rounding. A result that
// rounds to infinity is refused rather than published as a state value.
// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:269-277
std::uint16_t float_to_half(float value) {
    const auto bits = std::bit_cast<std::uint32_t>(value);
    const auto sign = std::uint16_t((bits >> 16) & 0x8000u);
    const auto exponent = (bits >> 23) & 0xffu;
    const auto fraction = bits & 0x7fffffu;
    if (exponent == 0) return 0;  // binary32 subnormals are below half's tie
    const int power = int(exponent) - 127;
    if (power > 15) throw std::overflow_error("scalar_half_overflow");
    if (power < -25) return 0;
    if (power < -14) {
        const auto rounded = round_shift_even(0x800000u | fraction,
                                               unsigned(-power - 1));
        // 0x400 carries into min normal; zero has one canonical sign.
        return rounded == 0 ? 0 : sign | std::uint16_t(rounded);
    }
    std::uint32_t rounded = round_shift_even(fraction, 13);
    std::uint32_t half_exponent = std::uint32_t(power + 15);
    if (rounded == 0x400u) {
        rounded = 0;
        ++half_exponent;
    }
    if (half_exponent >= 31) throw std::overflow_error("scalar_half_overflow");
    return sign | std::uint16_t((half_exponent << 10) | rounded);
}

// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:269-277
std::uint16_t float_to_bfloat(float value) {
    const auto bits = std::bit_cast<std::uint32_t>(value);
    const auto upper = bits >> 16;
    const auto lower = bits & 0xffffu;
    const auto rounded = upper + (lower > 0x8000u ||
                                  (lower == 0x8000u && (upper & 1u)));
    if ((rounded & 0x7f80u) == 0x7f80u)
        throw std::overflow_error("scalar_bfloat_overflow");
    return (rounded & 0x7fffu) == 0 ? 0 : std::uint16_t(rounded);
}

// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:269-277
void require_width(ScalarType type, std::size_t length) {
    if (length != scalar_width(type))
        throw std::invalid_argument("scalar_codec_width_mismatch");
}

}  // namespace

// SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:263-321
float read_scalar32(ScalarType type, std::span<const std::byte> bytes) {
    if (type == ScalarType::float64)
        throw std::invalid_argument("scalar_codec_f64_requires_binary64");
    require_width(type, bytes.size());
    const auto bits = read_le(bytes);
    float value = 0;
    switch (type) {
        case ScalarType::bfloat16:
            value = std::bit_cast<float>(std::uint32_t(bits) << 16);
            break;
        case ScalarType::float16:
            value = half_to_float(std::uint16_t(bits));
            break;
        case ScalarType::float32:
            value = std::bit_cast<float>(std::uint32_t(bits));
            break;
        case ScalarType::float64:
            throw std::invalid_argument("scalar_codec_f64_requires_binary64");
    }
    if (!std::isfinite(value)) throw std::invalid_argument("scalar_codec_nonfinite");
    return value == 0 ? 0.0f : value;
}

// SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:263-321
double read_scalar64(ScalarType type, std::span<const std::byte> bytes) {
    if (type != ScalarType::float64)
        throw std::invalid_argument("scalar_codec_binary64_requires_f64");
    require_width(type, bytes.size());
    const double value = std::bit_cast<double>(read_le(bytes));
    if (!std::isfinite(value)) throw std::invalid_argument("scalar_codec_nonfinite");
    return value == 0 ? 0.0 : value;
}

// SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:238-262
bool try_read_scalar32(const CognitiveTensor& tensor, std::size_t element,
                       float& value) noexcept {
    const auto type = tensor.scalar_type();
    if (type == ScalarType::float64 || element >= tensor.element_count()) return false;
    const std::size_t width = type == ScalarType::float32 ? 4 :
                              (type == ScalarType::float16 || type == ScalarType::bfloat16) ? 2 : 0;
    if (width == 0 || element >= tensor.byte_count() / width) return false;
    try {
        std::array<std::byte, 4> bytes{};
        tensor.copy_bytes(element * width, std::span<std::byte>(bytes).first(width));
        value = read_scalar32(type, std::span<const std::byte>(bytes).first(width));
        return true;
    } catch (...) {
        return false;
    }
}

// SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:238-262
bool try_read_scalar64(const CognitiveTensor& tensor, std::size_t element,
                       double& value) noexcept {
    if (tensor.scalar_type() != ScalarType::float64 ||
        element >= tensor.element_count() || element >= tensor.byte_count() / 8)
        return false;
    try {
        std::array<std::byte, 8> bytes{};
        tensor.copy_bytes(element * 8, bytes);
        value = read_scalar64(ScalarType::float64, bytes);
        return true;
    } catch (...) {
        return false;
    }
}

// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:269-277
void write_scalar32(ScalarType type, float value, std::span<std::byte> bytes) {
    if (type == ScalarType::float64)
        throw std::invalid_argument("scalar_codec_f64_requires_binary64");
    require_width(type, bytes.size());
    if (!std::isfinite(value)) throw std::invalid_argument("scalar_codec_nonfinite");
    if (value == 0) value = 0;  // canonical positive zero
    switch (type) {
        case ScalarType::bfloat16:
            write_le(float_to_bfloat(value), bytes);
            return;
        case ScalarType::float16:
            write_le(float_to_half(value), bytes);
            return;
        case ScalarType::float32:
            write_le(std::bit_cast<std::uint32_t>(value), bytes);
            return;
        case ScalarType::float64:
            break;
    }
    throw std::invalid_argument("scalar_codec_type_invalid");
}

// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:269-277
void write_scalar64(ScalarType type, double value, std::span<std::byte> bytes) {
    if (type != ScalarType::float64)
        throw std::invalid_argument("scalar_codec_binary64_requires_f64");
    require_width(type, bytes.size());
    if (!std::isfinite(value)) throw std::invalid_argument("scalar_codec_nonfinite");
    if (value == 0) value = 0;
    write_le(std::bit_cast<std::uint64_t>(value), bytes);
}

}  // namespace swegca::architecture
