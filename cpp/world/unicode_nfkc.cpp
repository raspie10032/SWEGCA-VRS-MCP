#include "world/unicode_nfkc.hpp"

#include "world/unicode_nfkc_data.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace swegca::world {
namespace {

constexpr char32_t hangul_s_base = 0xac00;
constexpr char32_t hangul_l_base = 0x1100;
constexpr char32_t hangul_v_base = 0x1161;
constexpr char32_t hangul_t_base = 0x11a7;
constexpr char32_t hangul_l_count = 19;
constexpr char32_t hangul_v_count = 21;
constexpr char32_t hangul_t_count = 28;
constexpr char32_t hangul_n_count = hangul_v_count * hangul_t_count;
constexpr char32_t hangul_s_count = hangul_l_count * hangul_n_count;

std::vector<char32_t> decode_utf8(const std::string_view input) {
    std::vector<char32_t> result;
    result.reserve(input.size());
    for (std::size_t at = 0; at != input.size();) {
        const auto first = static_cast<unsigned char>(input[at]);
        std::size_t size = 0;
        char32_t point = 0;
        char32_t minimum = 0;
        if (first < 0x80U) {
            size = 1; point = first;
        } else if ((first & 0xe0U) == 0xc0U) {
            size = 2; point = first & 0x1fU; minimum = 0x80;
        } else if ((first & 0xf0U) == 0xe0U) {
            size = 3; point = first & 0x0fU; minimum = 0x800;
        } else if ((first & 0xf8U) == 0xf0U) {
            size = 4; point = first & 0x07U; minimum = 0x10000;
        } else {
            throw std::invalid_argument("NFKC input contains invalid UTF-8");
        }
        if (size > input.size() - at) {
            throw std::invalid_argument("NFKC input contains truncated UTF-8");
        }
        for (std::size_t index = 1; index != size; ++index) {
            const auto byte = static_cast<unsigned char>(input[at + index]);
            if ((byte & 0xc0U) != 0x80U) {
                throw std::invalid_argument("NFKC input contains invalid UTF-8 continuation");
            }
            point = (point << 6U) | (byte & 0x3fU);
        }
        if ((size != 1 && point < minimum) || point > 0x10ffffU ||
            (point >= 0xd800U && point <= 0xdfffU)) {
            throw std::invalid_argument("NFKC input contains invalid Unicode scalar");
        }
        result.push_back(point);
        at += size;
    }
    return result;
}

std::uint8_t combining_class(const char32_t point) {
    const auto iterator = std::lower_bound(
        unicode_data::combining_classes.begin(),
        unicode_data::combining_classes.end(), point,
        [](const auto& entry, const char32_t value) { return entry.point < value; });
    return iterator != unicode_data::combining_classes.end() && iterator->point == point
        ? iterator->value : 0;
}

void decompose(const char32_t point, std::vector<char32_t>& output) {
    const auto hangul_index = point - hangul_s_base;
    if (hangul_index < hangul_s_count) {
        const auto l = hangul_l_base + hangul_index / hangul_n_count;
        const auto v = hangul_v_base + (hangul_index % hangul_n_count) / hangul_t_count;
        const auto t = hangul_t_base + hangul_index % hangul_t_count;
        output.push_back(l);
        output.push_back(v);
        if (t != hangul_t_base) output.push_back(t);
        return;
    }
    const auto iterator = std::lower_bound(
        unicode_data::decompositions.begin(), unicode_data::decompositions.end(), point,
        [](const auto& entry, const char32_t value) { return entry.point < value; });
    if (iterator == unicode_data::decompositions.end() || iterator->point != point) {
        output.push_back(point);
        return;
    }
    output.insert(output.end(),
        unicode_data::decomposition_values.begin() + iterator->offset,
        unicode_data::decomposition_values.begin() + iterator->offset + iterator->size);
}

char32_t compose_pair(const char32_t first, const char32_t second) {
    if (first >= hangul_l_base && first < hangul_l_base + hangul_l_count &&
        second >= hangul_v_base && second < hangul_v_base + hangul_v_count) {
        return hangul_s_base +
            ((first - hangul_l_base) * hangul_v_count + second - hangul_v_base) *
                hangul_t_count;
    }
    if (first >= hangul_s_base && first < hangul_s_base + hangul_s_count &&
        (first - hangul_s_base) % hangul_t_count == 0 &&
        second > hangul_t_base && second < hangul_t_base + hangul_t_count) {
        return first + second - hangul_t_base;
    }
    const auto key = std::pair{first, second};
    const auto iterator = std::lower_bound(
        unicode_data::compositions.begin(), unicode_data::compositions.end(), key,
        [](const auto& entry, const auto& value) {
            return std::pair{entry.first, entry.second} < value;
        });
    return iterator != unicode_data::compositions.end() &&
        iterator->first == first && iterator->second == second ? iterator->value : 0;
}

void append_utf8(std::string& output, const char32_t point) {
    if (point <= 0x7fU) {
        output.push_back(static_cast<char>(point));
    } else if (point <= 0x7ffU) {
        output.push_back(static_cast<char>(0xc0U | (point >> 6U)));
        output.push_back(static_cast<char>(0x80U | (point & 0x3fU)));
    } else if (point <= 0xffffU) {
        output.push_back(static_cast<char>(0xe0U | (point >> 12U)));
        output.push_back(static_cast<char>(0x80U | ((point >> 6U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | (point & 0x3fU)));
    } else {
        output.push_back(static_cast<char>(0xf0U | (point >> 18U)));
        output.push_back(static_cast<char>(0x80U | ((point >> 12U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | ((point >> 6U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | (point & 0x3fU)));
    }
}

}  // namespace

std::string normalize_nfkc(const std::string_view input) {
    const auto decoded = decode_utf8(input);
    std::vector<char32_t> decomposed;
    decomposed.reserve(decoded.size());
    for (const auto point : decoded) decompose(point, decomposed);

    // Canonical ordering. Each insertion only crosses earlier non-starters with
    // a greater combining class, matching Unicode normalization rule D109.
    for (std::size_t index = 1; index < decomposed.size(); ++index) {
        const auto current_class = combining_class(decomposed[index]);
        if (current_class == 0) continue;
        std::size_t insertion = index;
        while (insertion > 0) {
            const auto prior_class = combining_class(decomposed[insertion - 1]);
            if (prior_class == 0 || prior_class <= current_class) break;
            std::swap(decomposed[insertion], decomposed[insertion - 1]);
            --insertion;
        }
    }

    std::vector<char32_t> composed;
    composed.reserve(decomposed.size());
    std::size_t starter_position = 0;
    char32_t starter = 0;
    std::uint8_t previous_class = 0;
    for (const auto point : decomposed) {
        const auto current_class = combining_class(point);
        const auto composite = composed.empty() ? 0 : compose_pair(starter, point);
        if (composite != 0 && (previous_class < current_class || previous_class == 0)) {
            composed[starter_position] = composite;
            starter = composite;
            continue;
        }
        if (current_class == 0) {
            starter_position = composed.size();
            starter = point;
        }
        composed.push_back(point);
        previous_class = current_class;
    }

    std::string output;
    output.reserve(input.size());
    for (const auto point : composed) append_utf8(output, point);
    return output;
}

}  // namespace swegca::world
