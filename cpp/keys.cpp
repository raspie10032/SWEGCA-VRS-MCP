#include "keys.hpp"

#include "unicode.hpp"
#include "unicode_tables.hpp"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace swegca::vrs {
namespace {

// SWEGCA: src/swegca_vrs2/store.py@7536139:72-79
bool python_word(std::uint32_t point) {
    std::size_t low = 0;
    std::size_t high = std::size(unicode_table::word_ranges);
    while (low < high) {
        const auto middle = low + (high - low) / 2;
        const auto range = unicode_table::word_ranges[middle];
        if (point < range.first) high = middle;
        else if (point > range.last) low = middle + 1;
        else return true;
    }
    return false;
}

// SWEGCA: src/swegca_vrs2/store.py@7536139:72-79
std::vector<std::uint32_t> python_casefold(std::string_view text) {
    const auto original = decode_utf8(text);
    std::vector<std::uint32_t> folded;
    folded.reserve(original.size());
    for (const auto point : original) {
        const auto* first = std::begin(unicode_table::fold_mappings);
        const auto* last = std::end(unicode_table::fold_mappings);
        const auto* found = std::lower_bound(first, last, point,
            [](const auto& mapping, std::uint32_t key) { return mapping.point < key; });
        if (found == last || found->point != point) {
            folded.push_back(point);
        } else {
            for (std::uint32_t i = 0; i < found->count; ++i)
                folded.push_back(unicode_table::fold_codepoints[found->offset + i]);
        }
    }
    return folded;
}

// SWEGCA: src/swegca_vrs2/store.py@7536139:72-79
void retain_token(std::vector<std::string>& result,
                  std::vector<std::vector<std::uint32_t>>& original_words,
                  std::unordered_set<std::string>& seen,
                  std::vector<std::uint32_t>& token) {
    if (token.empty()) return;
    std::string encoded;
    encoded.reserve(token.size() * 2);
    for (const auto point : token) append_utf8(encoded, point);
    if (seen.emplace(encoded).second) {
        result.push_back(std::move(encoded));
        original_words.push_back(std::move(token));
    }
    token.clear();
}

}  // namespace

// SWEGCA: src/swegca_vrs2/store.py@7536139:72-79
std::vector<std::string> lexical_keys(std::string_view text) {
    const auto folded = python_casefold(text);
    std::vector<std::string> result;
    std::vector<std::vector<std::uint32_t>> original_words;
    std::unordered_set<std::string> seen;
    std::vector<std::uint32_t> token;
    for (const auto point : folded) {
        if (python_word(point)) token.push_back(point);
        else retain_token(result, original_words, seen, token);
    }
    retain_token(result, original_words, seen, token);
    for (const auto& word : original_words) {
        if (!std::all_of(word.begin(), word.end(), [](auto point) {
                return point >= 0xac00 && point <= 0xd7a3;
            })) continue;
        for (std::size_t size = 2; size <= std::min<std::size_t>(4, word.size()); ++size) {
            for (std::size_t at = 0; at + size <= word.size(); ++at) {
                std::string piece;
                for (std::size_t i = at; i < at + size; ++i)
                    append_utf8(piece, word[i]);
                if (seen.emplace(piece).second) result.push_back(std::move(piece));
            }
        }
    }
    return result;
}

// SWEGCA: src/swegca_vrs2/store.py@7536139:154-154
std::string casefold_text(std::string_view text) {
    const auto folded = python_casefold(text);
    std::string result;
    for (const auto point : folded) append_utf8(result, point);
    return result;
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_memory_activation.py@7536139:22-23
std::string normalize_cue(std::string_view cue) {
    const auto original = decode_utf8(cue);
    std::size_t first = 0;
    while (first < original.size() && python_space(original[first])) ++first;
    std::size_t last = original.size();
    while (last > first && python_space(original[last - 1])) --last;
    if (first == last) throw std::runtime_error("cue must not be empty");
    std::string collapsed;
    bool previous_space = false;
    for (std::size_t at = first; at < last; ++at) {
        if (python_space(original[at])) {
            if (!previous_space) collapsed.push_back(' ');
            previous_space = true;
        } else {
            append_utf8(collapsed, original[at]);
            previous_space = false;
        }
    }
    return casefold_text(collapsed);
}

}  // namespace swegca::vrs
