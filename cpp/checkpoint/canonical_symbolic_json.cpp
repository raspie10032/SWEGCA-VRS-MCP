#include "checkpoint/canonical_symbolic_json.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <string_view>
#include <unordered_set>

namespace swegca::checkpoint {
namespace {

[[noreturn]] void reject(std::string_view reason) {
    throw RestrictedPickleError(std::string(reason));
}

bool valid_utf8(std::string_view value) noexcept {
    std::size_t at = 0;
    while (at < value.size()) {
        const auto first = static_cast<unsigned char>(value[at++]);
        if (first <= 0x7f) continue;
        unsigned continuation = 0;
        std::uint32_t codepoint = 0;
        std::uint32_t minimum = 0;
        if (first >= 0xc2 && first <= 0xdf) {
            continuation = 1; codepoint = first & 0x1fU; minimum = 0x80;
        } else if (first >= 0xe0 && first <= 0xef) {
            continuation = 2; codepoint = first & 0x0fU; minimum = 0x800;
        } else if (first >= 0xf0 && first <= 0xf4) {
            continuation = 3; codepoint = first & 0x07U; minimum = 0x10000;
        } else {
            return false;
        }
        if (continuation > value.size() - at) return false;
        for (unsigned index = 0; index < continuation; ++index) {
            const auto next = static_cast<unsigned char>(value[at++]);
            if ((next & 0xc0U) != 0x80U) return false;
            codepoint = (codepoint << 6) | (next & 0x3fU);
        }
        if (codepoint < minimum || codepoint > 0x10ffffU ||
            (codepoint >= 0xd800U && codepoint <= 0xdfffU)) return false;
    }
    return true;
}

std::string python_float(double value) {
    if (!std::isfinite(value)) reject("canonical JSON rejects non-finite real values");
    char buffer[128];
    const auto converted = std::to_chars(std::begin(buffer), std::end(buffer), value,
                                         std::chars_format::general);
    if (converted.ec != std::errc{}) reject("binary64 shortest conversion failed");
    std::string result(buffer, converted.ptr);
    const auto exponent_at = result.find('e');
    if (exponent_at == std::string::npos) {
        if (result.find('.') == std::string::npos) result += ".0";
        return result;
    }

    int exponent = 0;
    auto exponent_text = std::string_view(result).substr(exponent_at + 1);
    bool exponent_negative = false;
    if (!exponent_text.empty() && (exponent_text.front() == '+' || exponent_text.front() == '-')) {
        exponent_negative = exponent_text.front() == '-';
        exponent_text.remove_prefix(1);
    }
    unsigned exponent_magnitude = 0;
    const auto parsed = std::from_chars(exponent_text.data(), exponent_text.data() + exponent_text.size(),
                                        exponent_magnitude);
    if (exponent_text.empty() || parsed.ec != std::errc{} ||
        parsed.ptr != exponent_text.data() + exponent_text.size() ||
        exponent_magnitude > static_cast<unsigned>(std::numeric_limits<int>::max()))
        reject("binary64 exponent conversion failed");
    exponent = static_cast<int>(exponent_magnitude);
    if (exponent_negative) exponent = -exponent;

    // Python repr/json uses fixed notation for decimal exponents -4..15.
    if (exponent >= -4 && exponent < 16) {
        const bool negative = result.front() == '-';
        const auto mantissa_begin = negative ? 1U : 0U;
        std::string digits;
        for (std::size_t index = mantissa_begin; index < exponent_at; ++index)
            if (result[index] != '.') digits.push_back(result[index]);
        const auto decimal = std::int64_t{1} + exponent;
        std::string fixed = negative ? "-" : "";
        if (decimal <= 0) {
            fixed += "0.";
            fixed.append(static_cast<std::size_t>(-decimal), '0');
            fixed += digits;
        } else if (static_cast<std::size_t>(decimal) >= digits.size()) {
            fixed += digits;
            fixed.append(static_cast<std::size_t>(decimal) - digits.size(), '0');
            fixed += ".0";
        } else {
            fixed.append(digits, 0, static_cast<std::size_t>(decimal));
            fixed.push_back('.');
            fixed.append(digits, static_cast<std::size_t>(decimal), std::string::npos);
        }
        return fixed;
    }

    // Python always spells a scientific exponent with a sign and at least two
    // digits. Normalize instead of depending on one library's to_chars style.
    std::string scientific = result.substr(0, exponent_at + 1);
    scientific.push_back(exponent < 0 ? '-' : '+');
    const auto magnitude = static_cast<unsigned>(exponent < 0 ? -static_cast<long long>(exponent) : exponent);
    char digits[32];
    const auto encoded = std::to_chars(std::begin(digits), std::end(digits), magnitude);
    const auto count = static_cast<std::size_t>(encoded.ptr - digits);
    if (count < 2) scientific.push_back('0');
    scientific.append(digits, encoded.ptr);
    return scientific;
}

class Encoder final {
public:
    explicit Encoder(const CanonicalSymbolicJsonLimits& limits) : limits_(limits) {
        if (!limits_.maximum_depth || !limits_.maximum_output_bytes)
            reject("canonical JSON limits must be nonzero");
    }

    std::string encode(const Symbolic& value) {
        emit(value, 0);
        return std::move(output_);
    }

private:
    class Active final {
    public:
        Active(std::unordered_set<const SymbolicValue*>& active, const SymbolicValue* value)
            : active_(active), value_(value) {
            if (!active_.insert(value_).second) reject("canonical JSON rejects cyclic symbolic graphs");
        }
        ~Active() { active_.erase(value_); }
        Active(const Active&) = delete;
        Active& operator=(const Active&) = delete;
    private:
        std::unordered_set<const SymbolicValue*>& active_;
        const SymbolicValue* value_;
    };

    void append(std::string_view value) {
        if (value.size() > limits_.maximum_output_bytes - output_.size())
            reject("canonical JSON output limit exceeded");
        output_.append(value);
    }
    void character(char value) { append(std::string_view(&value, 1)); }

    void string(std::string_view value) {
        if (!valid_utf8(value)) reject("canonical JSON string is not valid UTF-8");
        character('"');
        static constexpr char hex[] = "0123456789abcdef";
        for (const auto raw : value) {
            const auto byte = static_cast<unsigned char>(raw);
            switch (byte) {
            case '"': append("\\\""); break;
            case '\\': append("\\\\"); break;
            case '\b': append("\\b"); break;
            case '\t': append("\\t"); break;
            case '\n': append("\\n"); break;
            case '\f': append("\\f"); break;
            case '\r': append("\\r"); break;
            default:
                if (byte < 0x20) {
                    char escaped[] = {'\\', 'u', '0', '0', hex[byte >> 4], hex[byte & 0xf]};
                    append(std::string_view(escaped, sizeof escaped));
                } else {
                    character(raw);
                }
            }
        }
        character('"');
    }

    void emit(const Symbolic& value, std::size_t depth) {
        if (!value) reject("canonical JSON rejects a null symbolic pointer");
        if (depth > limits_.maximum_depth) reject("canonical JSON nesting limit exceeded");
        switch (value->kind) {
        case SymbolicKind::none: append("null"); return;
        case SymbolicKind::boolean: append(value->boolean ? "true" : "false"); return;
        case SymbolicKind::integer: {
            char buffer[32];
            const auto result = std::to_chars(std::begin(buffer), std::end(buffer), value->integer);
            if (result.ec != std::errc{}) reject("integer conversion failed");
            append(std::string_view(buffer, result.ptr));
            return;
        }
        case SymbolicKind::real: append(python_float(value->real)); return;
        case SymbolicKind::string: string(value->text); return;
        case SymbolicKind::list: {
            Active guard(active_, value.get());
            character('[');
            for (std::size_t index = 0; index < value->sequence.size(); ++index) {
                if (index) character(',');
                emit(value->sequence[index], depth + 1);
            }
            character(']');
            return;
        }
        case SymbolicKind::dictionary: {
            Active guard(active_, value.get());
            std::vector<const std::pair<Symbolic, Symbolic>*> entries;
            entries.reserve(value->entries.size());
            std::unordered_set<std::string_view> keys;
            for (const auto& entry : value->entries) {
                if (!entry.first || entry.first->kind != SymbolicKind::string)
                    reject("canonical JSON dictionary key is not a string");
                if (!valid_utf8(entry.first->text)) reject("canonical JSON dictionary key is not valid UTF-8");
                if (!keys.insert(entry.first->text).second)
                    reject("canonical JSON dictionary contains a duplicate key");
                entries.push_back(&entry);
            }
            std::sort(entries.begin(), entries.end(), [](const auto* left, const auto* right) {
                // Valid UTF-8 byte order is the same as Unicode scalar order.
                return left->first->text < right->first->text;
            });
            character('{');
            for (std::size_t index = 0; index < entries.size(); ++index) {
                if (index) character(',');
                string(entries[index]->first->text);
                character(':');
                emit(entries[index]->second, depth + 1);
            }
            character('}');
            return;
        }
        default:
            reject("symbolic pickle kind is not JSON-compatible");
        }
    }

    const CanonicalSymbolicJsonLimits& limits_;
    std::string output_;
    std::unordered_set<const SymbolicValue*> active_;
};

}  // namespace

std::string canonical_symbolic_json(const Symbolic& value,
                                    const CanonicalSymbolicJsonLimits& limits) {
    return Encoder(limits).encode(value);
}

}  // namespace swegca::checkpoint
