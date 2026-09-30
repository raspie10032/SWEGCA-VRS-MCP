#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view mosaic_phase1_source_sha256 =
    "5aa2f4373b92d0e6b31133e10ce3872f77034bf5fa03796f9c501639bd63324f";

struct Phase1Config final {
    std::uint64_t seed{29};
    std::uint64_t node_count{32};
    std::uint64_t workspace_slots{4};
    std::uint64_t model_dim{64};
    std::uint64_t attention_heads{4};
    std::uint64_t ffn_dim{128};
    std::uint64_t physical_layers{1};
    std::uint64_t reencode_interval{1};
    std::uint64_t train_depth{4};
    std::uint64_t eval_depth{8};
    std::uint64_t batch_size{256};
    std::uint64_t train_steps{1500};
    double learning_rate{2.0e-3};
    double min_seen_accuracy{0.95};
    double min_unseen_accuracy{0.80};
    double min_unseen_gain{0.05};
    double min_operator_gain{0.03};
    double min_operator_vs_rag_gain{0.03};
    double min_operator_latency_improvement{0.20};
    double rag_success_tolerance{0.01};

    void validate() const;
    [[nodiscard]] static Phase1Config target();
    bool operator==(const Phase1Config&) const = default;
};

enum class Phase1ModelKind : std::uint8_t { operator_model, text_rag };
enum class Phase1TensorDType : std::uint8_t { float32, int64 };
enum class Phase1Variant : std::uint8_t {
    recurrent,
    single_pass,
    no_operator,
    text_rag,
};

[[nodiscard]] std::string_view phase1_variant_name(Phase1Variant value) noexcept;
[[nodiscard]] Phase1Variant phase1_variant_from_name(std::string_view value);

struct Phase1Tensor final {
    std::string name;
    std::vector<std::uint64_t> shape;
    Phase1TensorDType dtype{Phase1TensorDType::float32};
    std::vector<float> float_values;
    std::vector<std::int64_t> integer_values;
};

using Phase1LogitTrace = std::vector<std::vector<float>>;

class SharedDepthSequenceModel final {
public:
    explicit SharedDepthSequenceModel(
        Phase1Config config, Phase1ModelKind kind = Phase1ModelKind::operator_model);
    ~SharedDepthSequenceModel();
    SharedDepthSequenceModel(SharedDepthSequenceModel&&) noexcept;
    SharedDepthSequenceModel& operator=(SharedDepthSequenceModel&&) noexcept;
    SharedDepthSequenceModel(const SharedDepthSequenceModel&) = delete;
    SharedDepthSequenceModel& operator=(const SharedDepthSequenceModel&) = delete;

    [[nodiscard]] const Phase1Config& config() const noexcept;
    [[nodiscard]] Phase1ModelKind kind() const noexcept;
    [[nodiscard]] Phase1LogitTrace forward_trace(
        std::span<const std::uint64_t> start_nodes,
        std::span<const std::uint64_t> operator_ids,
        std::span<const std::uint64_t> depths,
        bool recurrent) const;
    [[nodiscard]] std::vector<float> forward(
        std::span<const std::uint64_t> start_nodes,
        std::span<const std::uint64_t> operator_ids,
        std::span<const std::uint64_t> depths,
        bool recurrent) const;
    [[nodiscard]] std::uint64_t parameter_count() const noexcept;
    [[nodiscard]] std::vector<Phase1Tensor> state_dict() const;
    void load_state_dict(std::span<const Phase1Tensor> tensors);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
    friend struct Phase1NativeTrainingAccess;
};

[[nodiscard]] std::vector<std::uint64_t> ring_targets(
    std::span<const std::uint64_t> start_nodes,
    std::span<const std::uint64_t> operator_ids,
    std::span<const std::uint64_t> depths,
    std::uint64_t node_count);

struct Phase1DepthMetric final {
    std::uint64_t depth{};
    bool seen_in_training{};
    double accuracy{};
};

struct Phase1TrainingReport final {
    std::string mode;
    bool operator_visible{};
    std::string input_mode;
    std::uint64_t parameter_count{};
    double initial_loss{};
    double final_loss{};
    double minimum_loss{};
    double mean_last_100_loss{};
    double train_elapsed_seconds{};
    double train_steps_per_second{};
    double incremental_cuda_peak_allocated_mebibytes{};
    double process_cuda_peak_reserved_mebibytes{};
    std::vector<Phase1DepthMetric> metrics;
};

struct Phase1CheckpointReceipt final {
    std::filesystem::path checkpoint;
    std::filesystem::path manifest;
    std::string sha256;
    std::uint64_t bytes{};
};

struct Phase1LatencyComparison final {
    std::uint64_t depth{};
    std::uint64_t batch_size{1};
    std::uint64_t samples_per_model{};
    double operator_p50_milliseconds{};
    double text_rag_p50_milliseconds{};
    double improvement_fraction{};
};

struct Phase1MemoryOperatorSuite final {
    std::string namespace_name;
    std::uint64_t active_start_node{};
    std::uint64_t active_depth{};
    std::uint64_t start_history_count{};
    std::vector<std::string> history_values;
    std::vector<std::string> selected;
    std::vector<std::string> disabled;
    std::vector<std::vector<std::string>> conflicts;
    std::optional<std::uint64_t> prediction;
    std::optional<std::uint64_t> expected;
    std::uint64_t model_requests{};
    std::uint64_t forgotten_start_facts{};
    std::uint64_t active_fact_count_after_delete{};
    std::vector<std::string> remaining_predicates_after_delete;
    std::optional<std::uint64_t> remaining_depth;
    bool unknown_request_skipped_model{};
    std::map<std::string, bool, std::less<>> acceptance;
    bool passed{};
    std::string scope;
};

struct Phase1Comparison final {
    double recurrent_unseen{};
    double single_pass_unseen{};
    double no_operator_unseen{};
    double text_rag_unseen{};
    double recurrent_gain{};
    double operator_information_gain{};
    double operator_vs_text_rag_gain{};
    bool text_rag_matched_success{};
    Phase1LatencyComparison latency;
};

struct Phase1CompareReport final {
    std::string schema{"mosaic-phase1-v0"};
    Phase1Config config;
    std::string device{"cpu"};
    Phase1TrainingReport recurrent;
    Phase1TrainingReport single_pass;
    Phase1TrainingReport no_operator;
    Phase1TrainingReport text_rag;
    Phase1MemoryOperatorSuite memory_operator_suite;
    Phase1Comparison comparison;
    std::map<std::string, bool, std::less<>> acceptance;
    bool passed{};
    std::string scope{"synthetic token-sequence recurrence comparison; not LM quality"};
    std::optional<Phase1CheckpointReceipt> checkpoint;
    std::optional<bool> deprecated_text_rag_matched_success;
    std::optional<bool> source_passed;
    std::optional<std::string> reevaluation_reason;
};

struct Phase1VariantReport final {
    std::string schema{"mosaic-phase1-variant-v0"};
    Phase1Config config;
    std::string device{"cpu"};
    Phase1Variant variant{Phase1Variant::recurrent};
    Phase1TrainingReport result;
    std::map<std::string, bool, std::less<>> acceptance;
    bool passed{};
    std::string scope{"single trained Phase 1 variant; synthetic ring task only"};
    std::optional<Phase1CheckpointReceipt> checkpoint;
};

struct Phase1CheckpointVerification final {
    std::string schema{"mosaic-phase1-checkpoint-verification-v0"};
    std::filesystem::path checkpoint;
    std::filesystem::path manifest;
    std::string sha256;
    std::uint64_t bytes{};
    Phase1Variant variant{Phase1Variant::recurrent};
    Phase1Config config;
    std::string device{"cpu"};
    bool strict_state_dict{};
    bool all_tensors_loaded{};
    bool manifest_matches{};
    bool exhaustive{};
    std::uint64_t total_cases{};
    std::vector<Phase1DepthMetric> metrics;
    std::map<std::string, bool, std::less<>> acceptance;
    bool passed{};
    std::string scope{"checkpoint reload and exhaustive synthetic ring verification; not LM quality"};
};

struct Phase1ProfileReport final {
    std::string schema{"mosaic-phase1-profile-v0"};
    Phase1Config config;
    std::uint64_t parameter_count{};
    double bf16_mebibytes{};
    double int8_mebibytes{};
    double two_bit_mebibytes{};
    std::uint64_t target_parameter_minimum{20000000};
    std::uint64_t target_parameter_maximum{50000000};
    bool target_parameter_range{};
};

struct Phase1SeedAggregate final {
    double minimum_recurrent_unseen_mean_accuracy{};
    double minimum_recurrent_depth8_accuracy{};
    double minimum_operator_information_gain{};
    double minimum_latency_improvement{};
    bool all_passed{};
};

struct Phase1MultiSeedReport final {
    std::string schema{"mosaic-phase1-multiseed-v0"};
    std::vector<std::uint64_t> seeds;
    std::vector<Phase1CompareReport> runs;
    Phase1SeedAggregate aggregate;
    bool passed{};
    std::string scope{"three-seed synthetic recurrence gate; not LM quality"};
    std::optional<bool> source_passed;
    std::optional<std::string> reevaluation_reason;
};

[[nodiscard]] Phase1CompareReport compare_models(
    const Phase1Config& config = {}, std::string_view device = "auto",
    const std::optional<std::filesystem::path>& checkpoint_path = std::nullopt);
[[nodiscard]] Phase1CompareReport reevaluate_report(Phase1CompareReport report);
[[nodiscard]] Phase1MultiSeedReport reevaluate_report(Phase1MultiSeedReport report);
[[nodiscard]] Phase1VariantReport train_one_variant(
    Phase1Variant variant, const Phase1Config& config = {},
    std::string_view device = "auto",
    const std::optional<std::filesystem::path>& checkpoint_path = std::nullopt);
[[nodiscard]] Phase1CheckpointReceipt save_phase1_checkpoint(
    const SharedDepthSequenceModel& model, const Phase1Config& config,
    Phase1Variant variant, const std::filesystem::path& path);
[[nodiscard]] Phase1CheckpointVerification verify_checkpoint(
    const std::filesystem::path& path, std::string_view device = "auto");
[[nodiscard]] Phase1LatencyComparison compare_inference_latency(
    const SharedDepthSequenceModel& operator_model,
    const SharedDepthSequenceModel& text_rag_model, std::uint64_t depth,
    std::uint64_t warmups = 10, std::uint64_t repeats = 100,
    std::string_view device = "auto");
[[nodiscard]] Phase1MemoryOperatorSuite evaluate_memory_operator_suite(
    const SharedDepthSequenceModel& model, const Phase1Config& config,
    std::string_view device = "auto");
[[nodiscard]] Phase1ProfileReport profile_model(const Phase1Config& config);
[[nodiscard]] Phase1MultiSeedReport compare_seeds(
    const Phase1Config& config, std::span<const std::uint64_t> seeds,
    std::string_view device = "auto",
    const std::optional<std::filesystem::path>& checkpoint_directory = std::nullopt);

}  // namespace swegca::world
