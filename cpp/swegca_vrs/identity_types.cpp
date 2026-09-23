#include "swegca_vrs/identity_types.hpp"

#include <stdexcept>
#include <utility>

namespace swegca::vrs {
namespace {

// The author's _require_text uses Python str.strip. This C++ implementation
// fixes its blank set to Unicode 16.0; Python 3.14.7 measured these 29 code
// points. The source project permits other Python versions.
// The UTF-8 validity and byte-length limits below remain native rules.
// SWEGCA: src/swegca/mosaic_cognitive_kernel.py@5901a5a:19-21
bool python_strip_space(std::uint32_t code_point) noexcept {
    return (code_point >= 0x09 && code_point <= 0x0d) ||
           (code_point >= 0x1c && code_point <= 0x20) ||
           code_point == 0x85 || code_point == 0xa0 || code_point == 0x1680 ||
           (code_point >= 0x2000 && code_point <= 0x200a) ||
           code_point == 0x2028 || code_point == 0x2029 ||
           code_point == 0x202f || code_point == 0x205f ||
           code_point == 0x3000;
}

// Weak source analogy: implement the source's blank-text predicate on valid
// UTF-8 bytes; the source works on Python Unicode strings directly. Callers
// must run is_strict_utf8 first, since this decoder assumes complete scalars.
// SWEGCA: src/swegca/mosaic_cognitive_kernel.py@5901a5a:19-21
bool contains_identity_content(std::string_view value) noexcept {
    const auto* bytes = reinterpret_cast<const unsigned char*>(value.data());
    for (std::size_t at = 0; at < value.size();) {
        const std::uint32_t lead = bytes[at++];
        std::uint32_t code_point = lead;
        std::size_t continuations = 0;
        if (lead >= 0xf0) {
            code_point &= 0x07;
            continuations = 3;
        } else if (lead >= 0xe0) {
            code_point &= 0x0f;
            continuations = 2;
        } else if (lead >= 0xc0) {
            code_point &= 0x1f;
            continuations = 1;
        }
        for (std::size_t continuation = 0; continuation < continuations; ++continuation)
            code_point = (code_point << 6) | (bytes[at++] & 0x3f);
        if (!python_strip_space(code_point)) return true;
    }
    return false;
}

}  // namespace

namespace detail {

// Generalized UTF-8 is the native byte form of a Python str, including lone
// surrogate code points. It rejects overlong encodings, invalid continuations
// and code points above U+10FFFF. The parser and durable control use one rule.
// SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:114-120
bool push_generalized_utf8(GeneralizedUtf8State& state, std::byte value,
                           bool& complete) noexcept {
    complete = false;
    const auto byte = std::to_integer<std::uint32_t>(value);
    if (state.pending == 0) {
        if (byte < 0x80) {
            state.code_point = byte;
            complete = true;
            return true;
        }
        if (byte >= 0xc2 && byte <= 0xdf) {
            state.code_point = byte & 0x1f;
            state.minimum = 0x80;
            state.pending = 1;
        } else if (byte >= 0xe0 && byte <= 0xef) {
            state.code_point = byte & 0x0f;
            state.minimum = 0x800;
            state.pending = 2;
        } else if (byte >= 0xf0 && byte <= 0xf4) {
            state.code_point = byte & 0x07;
            state.minimum = 0x10000;
            state.pending = 3;
        } else {
            return false;
        }
        return true;
    }
    if ((byte & 0xc0) != 0x80) return false;
    state.code_point = (state.code_point << 6) | (byte & 0x3f);
    if (--state.pending != 0) return true;
    if (state.code_point < state.minimum || state.code_point > 0x10ffff) return false;
    complete = true;
    return true;
}

// SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:114-120
bool is_generalized_utf8(std::string_view value) noexcept {
    GeneralizedUtf8State state;
    for (unsigned char byte : value) {
        bool complete = false;
        if (!push_generalized_utf8(state, static_cast<std::byte>(byte), complete)) return false;
    }
    return state.pending == 0;
}

// Strict UTF-8 rejects overlong encodings, surrogate code points, truncated
// sequences, invalid continuations, and values above U+10FFFF. This byte
// validation is additional native infrastructure, not a Python _require_text
// operation.
// SWEGCA: src/swegca/mosaic_cognitive_kernel.py@5901a5a:19-21
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

// Native identity also imposes UTF-8, NUL and byte-length rules absent from
// the cited Python text check. Only the blank-string decision matches strip.
// SWEGCA: src/swegca/mosaic_cognitive_kernel.py@5901a5a:19-21
bool is_identity_text(std::string_view value) noexcept {
    return value.size() <= identity_text_max_bytes && is_strict_utf8(value) &&
           value.find('\0') == std::string_view::npos && contains_identity_content(value);
}

// SWEGCA: src/swegca/mosaic_cognitive_kernel.py@5901a5a:19-21
bool is_python_strip_space(std::uint32_t code_point) noexcept {
    return python_strip_space(code_point);
}

// SWEGCA: src/swegca/mosaic_cognitive_kernel.py@5901a5a:19-21
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

// SWEGCA: src/swegca/mosaic_evidence_accumulator.py@5901a5a:97-135
ClaimRevision::ClaimRevision(ClaimId claim, std::uint64_t revision)
    : claim_(std::move(claim)), revision_(revision) {
    if (revision_ == 0)
        throw std::invalid_argument("claim_revision_must_be_positive");
}

}  // namespace swegca::vrs
