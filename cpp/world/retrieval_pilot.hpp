#pragma once

#include "world/cognitive_state.hpp"

#include <cstddef>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace swegca::world {

struct RetrievalDocument final {
    std::string docid;
    std::string title;
    std::string text;
};

[[nodiscard]] std::vector<std::string> retrieval_text_features(std::string_view text);
[[nodiscard]] std::vector<std::string> load_retrieval_queries(
    const std::filesystem::path& topics_path);
[[nodiscard]] std::vector<RetrievalDocument> load_retrieval_corpus(
    const std::filesystem::path& corpus_path);

class Bm25Index final {
public:
    explicit Bm25Index(std::vector<RetrievalDocument> documents,
        double k1 = 1.2, double b = 0.75);
    [[nodiscard]] std::vector<std::pair<std::string, double>> search(
        std::string_view query, std::size_t top_k) const;
    [[nodiscard]] double k1() const noexcept { return k1_; }
    [[nodiscard]] double b() const noexcept { return b_; }
    [[nodiscard]] std::size_t feature_count() const noexcept { return postings_.size(); }
    [[nodiscard]] std::size_t document_count() const noexcept { return documents_.size(); }
private:
    std::vector<RetrievalDocument> documents_;
    double k1_{};
    double b_{};
    std::vector<std::size_t> lengths_;
    std::map<std::string, std::vector<std::pair<std::size_t, std::size_t>>, std::less<>> postings_;
    double average_length_{};
};

[[nodiscard]] JsonValue::Object evaluate_retrieval_pilot(
    const std::filesystem::path& topics_path,
    const std::filesystem::path& qrels_path,
    const std::filesystem::path& corpus_path,
    std::size_t top_k = 100);

inline constexpr std::string_view mosaic_retrieval_pilot_source_sha256 =
    "6e68ab236fedc497b6f85af1fb6bfac4702b73644d308487044b342a340e02d7";

}  // namespace swegca::world
