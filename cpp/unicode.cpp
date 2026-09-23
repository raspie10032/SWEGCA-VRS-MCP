#include "unicode.hpp"

#include "python_printable_ranges.hpp"

#include <algorithm>
#include <array>
#include <iterator>
#include <stdexcept>

namespace swegca::vrs {

// SWEGCA: src/swegca_vrs2/store.py@7536139:72-87
std::vector<std::uint32_t> decode_utf8(std::string_view bytes) {
    std::vector<std::uint32_t> result;
    result.reserve(bytes.size());
    for (std::size_t i = 0; i < bytes.size();) {
        const auto first = static_cast<unsigned char>(bytes[i]);
        std::uint32_t point = 0;
        std::size_t width = 0;
        std::uint32_t minimum = 0;
        if (first <= 0x7f) {
            point = first;
            width = 1;
        } else if (first >= 0xc2 && first <= 0xdf) {
            point = first & 0x1f;
            width = 2;
            minimum = 0x80;
        } else if (first >= 0xe0 && first <= 0xef) {
            point = first & 0x0f;
            width = 3;
            minimum = 0x800;
        } else if (first >= 0xf0 && first <= 0xf4) {
            point = first & 0x07;
            width = 4;
            minimum = 0x10000;
        } else throw std::runtime_error("invalid_utf8");
        if (width > bytes.size() - i) throw std::runtime_error("invalid_utf8");
        for (std::size_t offset = 1; offset < width; ++offset) {
            const auto next = static_cast<unsigned char>(bytes[i + offset]);
            if ((next & 0xc0) != 0x80) throw std::runtime_error("invalid_utf8");
            point = (point << 6) | (next & 0x3f);
        }
        if (point < minimum || point > 0x10ffff ||
            (point >= 0xd800 && point <= 0xdfff))
            throw std::runtime_error("invalid_utf8");
        result.push_back(point);
        i += width;
    }
    return result;
}

// SWEGCA: src/swegca_vrs2/store.py@7536139:72-79
void append_utf8(std::string& target, std::uint32_t point) {
    if (point <= 0x7f) target.push_back(static_cast<char>(point));
    else if (point <= 0x7ff) {
        target.push_back(static_cast<char>(0xc0 | (point >> 6)));
        target.push_back(static_cast<char>(0x80 | (point & 0x3f)));
    } else if (point <= 0xffff && (point < 0xd800 || point > 0xdfff)) {
        target.push_back(static_cast<char>(0xe0 | (point >> 12)));
        target.push_back(static_cast<char>(0x80 | ((point >> 6) & 0x3f)));
        target.push_back(static_cast<char>(0x80 | (point & 0x3f)));
    } else if (point >= 0x10000 && point <= 0x10ffff) {
        target.push_back(static_cast<char>(0xf0 | (point >> 18)));
        target.push_back(static_cast<char>(0x80 | ((point >> 12) & 0x3f)));
        target.push_back(static_cast<char>(0x80 | ((point >> 6) & 0x3f)));
        target.push_back(static_cast<char>(0x80 | (point & 0x3f)));
    } else throw std::runtime_error("invalid_unicode_codepoint");
}

// SWEGCA: src/swegca_vrs2/store.py@7536139:82-87
bool python_space(std::uint32_t point) {
    return (point >= 0x09 && point <= 0x0d) ||
           (point >= 0x1c && point <= 0x20) ||
           point == 0x85 || point == 0xa0 || point == 0x1680 ||
           (point >= 0x2000 && point <= 0x200a) ||
           point == 0x2028 || point == 0x2029 || point == 0x202f ||
           point == 0x205f || point == 0x3000;
}

namespace {

// SWEGCA: src/swegca_vrs2/store.py@7536139:149-153
bool python_printable(std::uint32_t point) {
    const auto* first = std::begin(unicode_table::printable_ranges);
    const auto* last = std::end(unicode_table::printable_ranges);
    const auto* found = std::lower_bound(
        first, last, point,
        [](const auto& range, std::uint32_t key) {
            return range.last < key;
        });
    return found != last && found->first <= point;
}

// SWEGCA: src/swegca_vrs2/store.py@7536139:149-153
void append_hex_escape(std::string& result, char prefix,
                       std::uint32_t point, std::size_t digits) {
    static constexpr std::array<char, 16> hex{
        '0', '1', '2', '3', '4', '5', '6', '7',
        '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    result.push_back('\\');
    result.push_back(prefix);
    for (std::size_t shift = digits; shift-- > 0;)
        result.push_back(hex[(point >> (shift * 4)) & 0xf]);
}

}  // namespace

// SWEGCA: src/swegca_vrs2/store.py@7536139:149-153
std::string python_key_error_text(std::string_view identifier) {
    const auto points = decode_utf8(identifier);
    const auto single = std::find(points.begin(), points.end(), '\'') !=
        points.end();
    const auto double_quote = std::find(points.begin(), points.end(), '"') !=
        points.end();
    const char quote = single && !double_quote ? '"' : '\'';
    std::string result(1, quote);
    for (const auto point : points) {
        if (point == '\\' || point == static_cast<std::uint32_t>(quote)) {
            result.push_back('\\');
            result.push_back(static_cast<char>(point));
        } else if (point == '\t') result += "\\t";
        else if (point == '\n') result += "\\n";
        else if (point == '\r') result += "\\r";
        else if (python_printable(point)) append_utf8(result, point);
        else if (point <= 0xff) append_hex_escape(result, 'x', point, 2);
        else if (point <= 0xffff) append_hex_escape(result, 'u', point, 4);
        else append_hex_escape(result, 'U', point, 8);
    }
    result.push_back(quote);
    return result;
}

}  // namespace swegca::vrs
