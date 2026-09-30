#pragma once

#include "world/retrieval_pilot.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <ostream>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view mosaic_byte_retriever_source_sha256 =
    "84a08b2dd4b41445255dfa34c5b706254e788a1eb62ce88d2767da18e9ec2f3b";

struct ByteRetrieverConfig final {
    std::int64_t seed{41};
    std::size_t patch_size{4};
    std::size_t model_dim{192};
    std::size_t attention_heads{6};
    std::size_t ffn_dim{384};
    std::size_t layers{2};
    std::size_t embedding_dim{128};
    std::size_t max_query_bytes{192};
    std::size_t max_document_bytes{768};
    std::size_t batch_size{32};
    std::size_t train_steps{1'000};
    double learning_rate{3e-4};
    double temperature{0.07};

    void validate() const;
    friend bool operator==(const ByteRetrieverConfig&, const ByteRetrieverConfig&) = default;
};

using ByteIdBatch = std::vector<std::vector<std::int64_t>>;
using FloatMatrix = std::vector<std::vector<double>>;

struct ByteTransformerLayerWeights final {
    std::vector<float> norm1_weight, norm1_bias;
    std::vector<float> attention_in_weight, attention_in_bias;
    std::vector<float> attention_out_weight, attention_out_bias;
    std::vector<float> norm2_weight, norm2_bias;
    std::vector<float> linear1_weight, linear1_bias;
    std::vector<float> linear2_weight, linear2_bias;
};

struct BytePatchRetrieverWeights final {
    std::vector<float> byte_embedding;
    std::vector<float> position_embedding;
    std::vector<ByteTransformerLayerWeights> encoder_layers;
    std::vector<float> output_norm_weight, output_norm_bias;
    std::vector<float> projection_weight;
};

class BytePatchRetriever final {
public:
    explicit BytePatchRetriever(ByteRetrieverConfig config);
    BytePatchRetriever(ByteRetrieverConfig config, BytePatchRetrieverWeights weights);

    [[nodiscard]] const ByteRetrieverConfig& config() const noexcept { return config_; }
    [[nodiscard]] const BytePatchRetrieverWeights& weights() const noexcept { return weights_; }
    [[nodiscard]] BytePatchRetrieverWeights& mutable_weights() noexcept { return weights_; }
    [[nodiscard]] FloatMatrix forward(const ByteIdBatch& byte_ids) const;
    [[nodiscard]] std::uint64_t parameter_count() const noexcept;

private:
    ByteRetrieverConfig config_;
    BytePatchRetrieverWeights weights_;
};

[[nodiscard]] ByteIdBatch encode_texts(const std::vector<std::string>& texts,
                                       std::size_t max_bytes,
                                       std::size_t patch_size);

struct RetrievalSplit final {
    std::map<std::string, std::string, std::less<>> topics;
    std::map<std::string, std::set<std::string, std::less<>>, std::less<>> positives;
    std::map<std::string, std::set<std::string, std::less<>>, std::less<>> negatives;
    std::map<std::string, std::string, std::less<>> documents;
};

[[nodiscard]] RetrievalSplit load_retrieval_split(
    const std::filesystem::path& topics_path,
    const std::filesystem::path& qrels_path,
    const std::filesystem::path& corpus_path);

struct RetrieverTrainingMetrics final {
    std::size_t eligible_queries{};
    double initial_loss{};
    double final_loss{};
    double mean_last_100_loss{};
    double elapsed_sec{};
    double steps_per_sec{};
    double cuda_peak_allocated_mib{};
    double cuda_peak_reserved_mib{};
};

struct RetrieverMetrics final {
    double mrr_at_10{};
    double ndcg_at_10{};
    double recall_at_10{};
    double recall_at_100{};
    friend bool operator==(const RetrieverMetrics&, const RetrieverMetrics&) = default;
};

struct RetrieverEvaluation final {
    std::size_t queries{};
    std::size_t documents{};
    std::size_t positive_qrels{};
    RetrieverMetrics metrics;
    double corpus_encode_sec{};
    double query_batch_and_search_sec{};
    double query_mean_ms{};
};

enum class RetrieverDevice : std::uint8_t { cpu, cuda };
[[nodiscard]] RetrieverDevice resolve_retriever_device(std::string_view value,
                                                        bool cuda_available = false);
[[nodiscard]] std::string_view retriever_device_name(RetrieverDevice device) noexcept;

[[nodiscard]] RetrieverTrainingMetrics train_retriever(
    BytePatchRetriever& model, const RetrievalSplit& split,
    const ByteRetrieverConfig& config, RetrieverDevice device = RetrieverDevice::cpu);
[[nodiscard]] RetrieverEvaluation evaluate_retriever(
    const BytePatchRetriever& model, const RetrievalSplit& split,
    const ByteRetrieverConfig& config, RetrieverDevice device = RetrieverDevice::cpu,
    std::size_t top_k = 100, std::size_t encode_batch_size = 64);
[[nodiscard]] RetrieverMetrics retrieval_metrics(
    const std::vector<std::string>& query_ids,
    const std::vector<std::vector<std::string>>& rankings,
    const std::map<std::string, std::set<std::string, std::less<>>, std::less<>>& positives);

struct ByteRetrieverCheckpointReceipt final {
    std::filesystem::path path;
    std::filesystem::path manifest_path;
    std::uint64_t bytes{};
    std::string sha256;
};

struct ByteRetrieverAcceptance final {
    bool parameter_count_at_most_5m{};
    bool checkpoint_at_most_25mb{};
    bool mrr_at_10_at_least_0_10{};
    bool recall_at_100_at_least_0_50{};
    bool mrr_gain_at_least_0_05{};
    bool loss_fell{};
    [[nodiscard]] bool passed() const noexcept;
};

struct ByteRetrieverExperimentReport final {
    std::string schema_version{"mosaic-byte-retriever-v0"};
    std::string scope{"MIRACL-ko judged-candidate mechanism gate; not full-corpus retrieval"};
    RetrieverDevice device{RetrieverDevice::cpu};
    ByteRetrieverConfig config;
    std::uint64_t parameter_count{};
    RetrieverEvaluation untrained_dev;
    RetrieverTrainingMetrics training;
    RetrieverEvaluation trained_dev;
    double mrr_at_10_gain{};
    double frozen_bm25_dev_mrr_at_10{0.574204};
    double frozen_bm25_dev_recall_at_100{0.939045};
    ByteRetrieverCheckpointReceipt checkpoint;
    ByteRetrieverAcceptance acceptance;
    std::vector<std::string> limitations;
    [[nodiscard]] bool passed() const noexcept { return acceptance.passed(); }
};

[[nodiscard]] ByteRetrieverCheckpointReceipt save_byte_retriever_checkpoint(
    const std::filesystem::path& path, const BytePatchRetriever& model,
    const ByteRetrieverConfig& config);

struct ByteRetrieverVerificationChecks final {
    bool manifest_digest{};
    bool manifest_size{};
    bool parameter_count_at_most_5m{};
    bool mrr_at_10_at_least_0_10{};
    bool recall_at_100_at_least_0_50{};
    std::optional<bool> matches_training_report;
    [[nodiscard]] bool passed() const noexcept;
};

struct ByteRetrieverVerificationReport final {
    std::string schema_version{"mosaic-byte-retriever-verification-v0"};
    std::filesystem::path checkpoint;
    std::filesystem::path manifest;
    std::string sha256;
    std::uint64_t bytes{};
    RetrieverDevice device{RetrieverDevice::cpu};
    ByteRetrieverConfig config;
    RetrieverEvaluation evaluation;
    std::optional<RetrieverMetrics> expected_metrics;
    ByteRetrieverVerificationChecks checks;
    [[nodiscard]] bool passed() const noexcept { return checks.passed(); }
};

[[nodiscard]] ByteRetrieverExperimentReport run_byte_retriever_experiment(
    const ByteRetrieverConfig& config, const RetrievalSplit& train_split,
    const RetrievalSplit& dev_split, RetrieverDevice device,
    const std::filesystem::path& checkpoint);
[[nodiscard]] ByteRetrieverVerificationReport verify_byte_retriever_checkpoint(
    const std::filesystem::path& path, const RetrievalSplit& dev_split,
    RetrieverDevice device,
    const std::optional<std::filesystem::path>& expected_report = std::nullopt);
[[nodiscard]] std::string byte_retriever_file_sha256(const std::filesystem::path& path);
[[nodiscard]] std::string byte_retriever_experiment_json(
    const ByteRetrieverExperimentReport& report);
[[nodiscard]] std::string byte_retriever_verification_json(
    const ByteRetrieverVerificationReport& report);
int run_byte_retriever_cli(std::span<const std::string_view> arguments,
                           std::ostream& output, std::ostream& errors);

}  // namespace swegca::world
