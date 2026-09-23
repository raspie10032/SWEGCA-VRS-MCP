#include "swegca_architecture/strong_types.hpp"

#include <stdexcept>
#include <utility>

namespace swegca::architecture {
namespace {

// C++ text identity uses an explicit ASCII-whitespace rule and does not copy
// Python Unicode strip behavior. The full byte sequence is always inspected.
// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:541-543
bool contains_identity_content(std::string_view value) {
    bool has_content = false;
    for (const auto byte : value) {
        if (byte != ' ' && byte != '\t' && byte != '\n' && byte != '\r' &&
            byte != '\f' && byte != '\v')
            has_content = true;
    }
    return has_content;
}

// SWEGCA: src/swegca/mosaic_evidence_revision.py@5901a5a:23-25
std::uint8_t hex_nibble(char value) {
    if (value >= '0' && value <= '9')
        return static_cast<std::uint8_t>(value - '0');
    if (value >= 'a' && value <= 'f')
        return static_cast<std::uint8_t>(value - 'a' + 10);
    throw std::invalid_argument("digest_must_be_lowercase_hex");
}

}  // namespace

namespace detail {

// Strict UTF-8 rejects overlong encodings, surrogate code points, truncated
// sequences, invalid continuations, and values above U+10FFFF.
// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:541-543
bool is_strict_utf8(std::string_view value) noexcept {
    const auto* bytes = reinterpret_cast<const unsigned char*>(value.data());
    std::size_t position = 0;
    while (position < value.size()) {
        const auto lead = static_cast<std::uint32_t>(bytes[position]);
        if (lead < 0x80) {
            ++position;
            continue;
        }

        std::size_t width = 0;
        std::uint32_t code_point = 0;
        std::uint32_t minimum = 0;
        if (lead >= 0xc2 && lead <= 0xdf) {
            width = 2;
            code_point = lead & 0x1f;
            minimum = 0x80;
        } else if (lead >= 0xe0 && lead <= 0xef) {
            width = 3;
            code_point = lead & 0x0f;
            minimum = 0x800;
        } else if (lead >= 0xf0 && lead <= 0xf4) {
            width = 4;
            code_point = lead & 0x07;
            minimum = 0x10000;
        } else {
            return false;
        }
        if (width > value.size() - position) return false;
        for (std::size_t offset = 1; offset < width; ++offset) {
            const auto continuation =
                static_cast<std::uint32_t>(bytes[position + offset]);
            if ((continuation & 0xc0) != 0x80) return false;
            code_point = (code_point << 6) | (continuation & 0x3f);
        }
        if (code_point < minimum || code_point > 0x10ffff ||
            (code_point >= 0xd800 && code_point <= 0xdfff))
            return false;
        position += width;
    }
    return true;
}

// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:541-543
bool is_identity_text(std::string_view value) noexcept {
    return value.size() <= identity_text_max_bytes && is_strict_utf8(value) &&
           value.find('\0') == std::string_view::npos && contains_identity_content(value);
}

// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:541-543
void require_identity_text(std::string_view value, std::string_view field) {
    if (value.size() > identity_text_max_bytes)
        throw std::invalid_argument(std::string(field) + "_too_long");
    if (!is_strict_utf8(value))
        throw std::invalid_argument(std::string(field) + "_must_be_utf8");
    if (value.find('\0') != std::string_view::npos)
        throw std::invalid_argument(std::string(field) + "_must_not_contain_nul");
    if (!contains_identity_content(value))
        throw std::invalid_argument(std::string(field) + "_must_not_be_empty");
}

}  // namespace detail

// SWEGCA: src/swegca/mosaic_evidence_revision.py@5901a5a:23-45
Digest256::Digest256(Bytes bytes) : bytes_(bytes) {}

// SWEGCA: src/swegca/mosaic_evidence_revision.py@5901a5a:23-25
Digest256 Digest256::from_hex(std::string_view value) {
    if (value.size() != width * 2)
        throw std::invalid_argument("digest_must_have_64_hex_characters");
    Bytes bytes{};
    for (std::size_t index = 0; index < width; ++index) {
        const auto high = hex_nibble(value[index * 2]);
        const auto low = hex_nibble(value[index * 2 + 1]);
        bytes[index] = std::byte{static_cast<std::uint8_t>((high << 4) | low)};
    }
    return Digest256(bytes);
}

// SWEGCA: src/swegca/mosaic_evidence_revision.py@5901a5a:15-25
std::string Digest256::hex() const {
    static constexpr char digits[] = "0123456789abcdef";
    std::string value(width * 2, '0');
    for (std::size_t index = 0; index < width; ++index) {
        const auto byte = std::to_integer<std::uint8_t>(bytes_[index]);
        value[index * 2] = digits[byte >> 4];
        value[index * 2 + 1] = digits[byte & 0x0f];
    }
    return value;
}

// SWEGCA: src/swegca/mosaic_bounded_world_write.py@5901a5a:175-188
StateGeneration::StateGeneration(std::uint64_t ordinal, Digest256 digest)
    : ordinal_(ordinal), digest_(std::move(digest)) {}

// SWEGCA: src/swegca/mosaic_evidence_accumulator.py@5901a5a:97-135
ClaimRevision::ClaimRevision(ClaimId claim, std::uint64_t revision)
    : claim_(std::move(claim)), revision_(revision) {
    if (revision_ == 0)
        throw std::invalid_argument("claim_revision_must_be_positive");
}

}  // namespace swegca::architecture
