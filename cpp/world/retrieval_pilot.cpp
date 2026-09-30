#include "world/retrieval_pilot.hpp"

#include "world/unicode_nfkc.hpp"
#include "world/unicode_word_data.hpp"
#include "world/unicode_whitespace_data.hpp"
#include "transport/json.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <fstream>
#include <map>
#include <memory_resource>
#include <set>
#include <sstream>
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

std::string read_text(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("cannot open retrieval input: " + path.string());
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

std::vector<std::string_view> lines(const std::string_view text) {
    std::vector<std::string_view> result;
    for (std::size_t begin = 0; begin < text.size();) {
        auto end = text.find('\n', begin);
        if (end == std::string_view::npos) end = text.size();
        auto line = text.substr(begin, end - begin);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (!line.empty()) result.push_back(line);
        begin = end == text.size() ? text.size() : end + 1;
    }
    return result;
}

std::pair<std::string_view, std::string_view> split_once(
    const std::string_view line, const char delimiter) {
    const auto at = line.find(delimiter);
    if (at == std::string_view::npos)
        throw std::invalid_argument("retrieval row is missing a required delimiter");
    return {line.substr(0, at), line.substr(at + 1)};
}

std::map<std::string, std::string, std::less<>> load_topics(
    const std::filesystem::path& path) {
    const auto content = read_text(path);
    std::map<std::string, std::string, std::less<>> result;
    for (const auto line : lines(content)) {
        const auto [identifier, query] = split_once(line, '\t');
        result[std::string(identifier)] = std::string(query);
    }
    return result;
}

std::map<std::string, std::set<std::string, std::less<>>, std::less<>> load_qrels(
    const std::filesystem::path& path) {
    const auto content = read_text(path);
    std::map<std::string, std::set<std::string, std::less<>>, std::less<>> result;
    for (const auto line : lines(content)) {
        std::array<std::string_view, 4> columns;
        std::size_t begin = 0;
        for (std::size_t index = 0; index < columns.size(); ++index) {
            const auto end = index + 1 == columns.size() ? line.size() : line.find('\t', begin);
            if (end == std::string_view::npos)
                throw std::invalid_argument("qrels row must contain four tab separated fields");
            columns[index] = line.substr(begin, end - begin);
            begin = end + 1;
        }
        std::size_t consumed = 0;
        const auto relevance = std::stoi(std::string(columns[3]), &consumed);
        if (consumed != columns[3].size()) throw std::invalid_argument("invalid qrels relevance");
        if (relevance > 0) result[std::string(columns[0])].insert(std::string(columns[2]));
    }
    return result;
}

std::vector<RetrievalDocument> load_corpus_rows(const std::filesystem::path& path) {
    const auto content = read_text(path);
    std::vector<RetrievalDocument> result;
    for (const auto line : lines(content)) {
        std::pmr::monotonic_buffer_resource memory;
        const auto row = transport::parse_json(line, memory);
        if (row.kind != transport::Json::Kind::object)
            throw std::invalid_argument("pilot corpus contains an invalid row");
        const auto* docid = row.find("docid");
        const auto* title = row.find("title");
        const auto* text = row.find("text");
        if (!docid || !title || !text)
            throw std::invalid_argument("pilot corpus contains an invalid row");
        result.push_back({std::string(docid->string()), std::string(title->string()),
                          std::string(text->string())});
    }
    return result;
}

double mean(const std::vector<double>& values) {
    double sum = 0.0;
    for (const auto value : values) sum += value;
    return values.empty() ? 0.0 : sum / static_cast<double>(values.size());
}

double percentile(const std::vector<double>& sorted, const double quantile) {
    if (sorted.empty()) return 0.0;
    const auto index = std::max<std::size_t>(
        static_cast<std::size_t>(std::ceil(quantile * sorted.size())), 1) - 1;
    return sorted[index];
}

double rounded_six(const double value) { return std::round(value * 1'000'000.0) / 1'000'000.0; }

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
        if (member(unicode_word_data::ranges, point)) word.push_back(point);
        else flush();
        if (!member(unicode_whitespace_data::ranges, point)) compact.push_back(point);
    }
    flush();
    if (compact.size() >= 3) for (std::size_t index = 0; index + 3 <= compact.size(); ++index)
        result.push_back("c:" + encode(compact, index, 3));
    return result;
}

std::vector<std::string> load_retrieval_queries(const std::filesystem::path& topics_path) {
    const auto topics = load_topics(topics_path);
    std::vector<std::string> result;
    result.reserve(topics.size());
    for (const auto& [_, query] : topics) result.push_back(query);
    return result;
}

std::vector<RetrievalDocument> load_retrieval_corpus(
    const std::filesystem::path& corpus_path) {
    return load_corpus_rows(corpus_path);
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

JsonValue::Object evaluate_retrieval_pilot(
    const std::filesystem::path& topics_path,
    const std::filesystem::path& qrels_path,
    const std::filesystem::path& corpus_path,
    const std::size_t top_k) {
    const auto topics = load_topics(topics_path);
    const auto positives = load_qrels(qrels_path);
    auto documents = load_corpus_rows(corpus_path);
    const auto build_started = std::chrono::steady_clock::now();
    const Bm25Index index(std::move(documents));
    const auto build_elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - build_started).count();

    std::vector<double> latencies, reciprocal_ranks, recalls_10, recalls_100, ndcgs_10;
    std::int64_t zero_score_queries = 0;
    std::int64_t positive_count = 0;
    for (const auto& [_, relevant] : positives) positive_count += relevant.size();
    bool all_have_qrels = true;
    for (const auto& [query_id, query] : topics) {
        const auto relevant_row = positives.find(query_id);
        all_have_qrels = all_have_qrels && relevant_row != positives.end();
        const std::set<std::string, std::less<>> empty;
        const auto& relevant = relevant_row == positives.end() ? empty : relevant_row->second;
        const auto started = std::chrono::steady_clock::now();
        const auto ranking = index.search(query, top_k);
        latencies.push_back(std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count());
        if (ranking.empty()) ++zero_score_queries;
        std::size_t first_rank = 0, hits_10 = 0, hits_100 = 0;
        double dcg = 0.0;
        for (std::size_t position = 0; position < ranking.size(); ++position) {
            if (!relevant.contains(ranking[position].first)) continue;
            const auto rank = position + 1;
            if (!first_rank) first_rank = rank;
            if (rank <= 10) { ++hits_10; dcg += 1.0 / std::log2(rank + 1.0); }
            if (rank <= 100) ++hits_100;
        }
        reciprocal_ranks.push_back(first_rank && first_rank <= 10 ? 1.0 / first_rank : 0.0);
        const auto denominator = static_cast<double>(relevant.size());
        recalls_10.push_back(denominator ? hits_10 / denominator : 0.0);
        recalls_100.push_back(denominator ? hits_100 / denominator : 0.0);
        double ideal = 0.0;
        for (std::size_t rank = 1; rank <= std::min<std::size_t>(relevant.size(), 10); ++rank)
            ideal += 1.0 / std::log2(rank + 1.0);
        ndcgs_10.push_back(ideal ? dcg / ideal : 0.0);
    }
    std::ranges::sort(latencies);
    const auto mrr = mean(reciprocal_ranks), ndcg = mean(ndcgs_10);
    const auto recall_10 = mean(recalls_10), recall_100 = mean(recalls_100);
    const auto metric_ranges = [&](const double value) { return value >= 0.0 && value <= 1.0; };
    const bool all_scored = zero_score_queries == 0;
    const bool ranges = metric_ranges(mrr) && metric_ranges(ndcg) &&
        metric_ranges(recall_10) && metric_ranges(recall_100);
    return {{"schema_version", "mosaic-retrieval-pilot-v1"},
        {"scope", "MIRACL-ko dev judged-candidate reranking baseline"},
        {"method", JsonValue::Object{{"name", "stdlib-bm25"},
            {"features", "NFKC casefolded words plus Unicode character trigrams"},
            {"k1", index.k1()}, {"b", index.b()}, {"top_k", static_cast<std::int64_t>(top_k)}}},
        {"counts", JsonValue::Object{{"queries", static_cast<std::int64_t>(topics.size())},
            {"documents", static_cast<std::int64_t>(index.document_count())},
            {"positive_qrels", positive_count},
            {"indexed_features", static_cast<std::int64_t>(index.feature_count())}}},
        {"metrics", JsonValue::Object{{"mrr_at_10", rounded_six(mrr)},
            {"ndcg_at_10", rounded_six(ndcg)}, {"recall_at_10", rounded_six(recall_10)},
            {"recall_at_100", rounded_six(recall_100)}, {"zero_score_queries", zero_score_queries}}},
        {"timing", JsonValue::Object{{"index_build_sec", rounded_six(build_elapsed)},
            {"query_mean_ms", rounded_six(mean(latencies))},
            {"query_p95_ms", rounded_six(percentile(latencies, 0.95))}}},
        {"checks", JsonValue::Object{{"all_queries_have_positive_qrels", all_have_qrels},
            {"all_queries_scored", all_scored}, {"metric_ranges", ranges}}},
        {"passed", all_have_qrels && all_scored && ranges},
        {"limitations", JsonValue::Array{
            "This is reranking over 2,835 judged documents, not retrieval over all 1,486,752 passages.",
            "The evaluation split is never used for training or parameter selection."}}};
}

}  // namespace swegca::world
