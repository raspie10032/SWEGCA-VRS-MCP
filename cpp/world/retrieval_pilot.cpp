#include "world/retrieval_pilot.hpp"

#include "world/unicode_nfkc.hpp"
#include "world/unicode_word_data.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>

namespace swegca::world {
namespace {

std::vector<char32_t> decode(const std::string_view input) {
    std::vector<char32_t> result;
    for (std::size_t at = 0; at < input.size();) {
        const auto first = static_cast<unsigned char>(input[at]);
        std::size_t size = 0; char32_t point = 0; char32_t minimum = 0;
        if (first < 0x80) { size = 1; point = first; }
        else if ((first & 0xe0) == 0xc0) { size = 2; point = first & 0x1f; minimum = 0x80; }
        else if ((first & 0xf0) == 0xe0) { size = 3; point = first & 0x0f; minimum = 0x800; }
        else if ((first & 0xf8) == 0xf0) { size = 4; point = first & 7; minimum = 0x10000; }
        else throw std::invalid_argument("retrieval text contains invalid UTF-8");
        if (size > input.size() - at) throw std::invalid_argument("retrieval text contains truncated UTF-8");
        for (std::size_t index = 1; index < size; ++index) {
            const auto byte = static_cast<unsigned char>(input[at + index]);
            if ((byte & 0xc0) != 0x80) throw std::invalid_argument("retrieval text contains invalid UTF-8");
            point = (point << 6) | (byte & 0x3f);
        }
        if ((size > 1 && point < minimum) || point > 0x10ffff ||
            (point >= 0xd800 && point <= 0xdfff))
            throw std::invalid_argument("retrieval text contains invalid Unicode scalar");
        result.push_back(point); at += size;
    }
    return result;
}

void append(std::string& output, const char32_t point) {
    if (point <= 0x7f) output.push_back(static_cast<char>(point));
    else if (point <= 0x7ff) {
        output.push_back(static_cast<char>(0xc0 | (point >> 6)));
        output.push_back(static_cast<char>(0x80 | (point & 0x3f)));
    } else if (point <= 0xffff) {
        output.push_back(static_cast<char>(0xe0 | (point >> 12)));
        output.push_back(static_cast<char>(0x80 | ((point >> 6) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | (point & 0x3f)));
    } else {
        output.push_back(static_cast<char>(0xf0 | (point >> 18)));
        output.push_back(static_cast<char>(0x80 | ((point >> 12) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | ((point >> 6) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | (point & 0x3f)));
    }
}

template <std::size_t Size>
bool member(const std::array<unicode_word_data::Range, Size>& ranges, const char32_t point) {
    const auto found = std::ranges::lower_bound(ranges, point, {},
        [](const auto& range) { return range.last; });
    return found != ranges.end() && found->first <= point;
}

std::string encode(const std::vector<char32_t>& points, const std::size_t first,
                   const std::size_t count) {
    std::string result;
    for (std::size_t index = first; index < first + count; ++index) append(result, points[index]);
    return result;
}

}  // namespace

std::vector<std::string> retrieval_text_features(const std::string_view text) {
    const auto normalized = unicode_casefold(normalize_nfkc(text));
    const auto points = decode(normalized);
    std::vector<std::string> result;
    std::vector<char32_t> word, compact;
    const auto flush = [&] {
        if (!word.empty()) { result.push_back("w:" + encode(word, 0, word.size())); word.clear(); }
    };
    for (const auto point : points) {
        if (member(unicode_word_data::word_ranges, point)) word.push_back(point);
        else flush();
        if (!member(unicode_word_data::whitespace_ranges, point)) compact.push_back(point);
    }
    flush();
    if (compact.size() >= 3) for (std::size_t index = 0; index + 3 <= compact.size(); ++index)
        result.push_back("c:" + encode(compact, index, 3));
    return result;
}

Bm25Index::Bm25Index(std::vector<RetrievalDocument> documents,
                     const double k1, const double b)
    : documents_(std::move(documents)), k1_(k1), b_(b) {
    if (documents_.empty()) throw std::invalid_argument("documents cannot be empty");
    for (std::size_t index = 0; index < documents_.size(); ++index) {
        std::map<std::string, std::size_t, std::less<>> counts;
        for (auto feature : retrieval_text_features(documents_[index].title + " " + documents_[index].text))
            ++counts[std::move(feature)];
        std::size_t length = 0;
        for (const auto& [feature, frequency] : counts) {
            length += frequency; postings_[feature].emplace_back(index, frequency);
        }
        lengths_.push_back(length); average_length_ += static_cast<double>(length);
    }
    average_length_ /= static_cast<double>(documents_.size());
}

std::vector<std::pair<std::string, double>> Bm25Index::search(
    const std::string_view query, const std::size_t top_k) const {
    std::map<std::string, std::size_t, std::less<>> query_counts;
    for (auto feature : retrieval_text_features(query)) ++query_counts[std::move(feature)];
    std::map<std::size_t, double> scores;
    for (const auto& [feature, query_frequency] : query_counts) {
        const auto found = postings_.find(feature);
        if (found == postings_.end()) continue;
        const auto document_frequency = static_cast<double>(found->second.size());
        const auto document_count = static_cast<double>(documents_.size());
        const auto inverse = std::log(1.0 + (document_count - document_frequency + 0.5) /
            (document_frequency + 0.5));
        for (const auto& [document, frequency] : found->second) {
            const auto length_ratio = lengths_[document] / average_length_;
            const auto denominator = frequency + k1_ * (1.0 - b_ + b_ * length_ratio);
            scores[document] += inverse * (frequency * (k1_ + 1.0) / denominator) * query_frequency;
        }
    }
    std::vector<std::pair<std::string, double>> result;
    for (const auto& [index, score] : scores) result.emplace_back(documents_[index].docid, score);
    std::ranges::sort(result, [](const auto& left, const auto& right) {
        return left.second != right.second ? left.second > right.second : left.first < right.first;
    });
    if (result.size() > top_k) result.resize(top_k);
    return result;
}

}  // namespace swegca::world
