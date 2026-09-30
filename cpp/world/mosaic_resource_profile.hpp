#pragma once

#include "world/cognitive_state.hpp"
#include "world/mosaic_phase1.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view mosaic_resource_profile_source_sha256 =
    "7880e17f92d382f75f1fd00cee7d1be8589984f0107e7dc86d1279d9ef221907";

struct ResourceProfileConfig final {
    std::uint64_t batch_size{1};
    std::vector<std::uint64_t> depths{1, 2, 4, 8};
    std::uint64_t warmups{3};
    std::uint64_t repeats{20};
    double max_combined_memory_mib{2048.0};
    void validate() const;
};

struct ProcessMemoryProfile final {
    double rss_mib{};
    double peak_rss_mib{};
    [[nodiscard]] JsonValue::Object to_dict() const;
};

struct DepthLatencyProfile final {
    std::uint64_t depth{};
    double p50_latency_ms{};
    double p95_latency_ms{};
    double mean_latency_ms{};
    std::uint64_t samples{};
    [[nodiscard]] JsonValue::Object to_dict() const;
};

struct ResourceProfileReport final {
    std::string schema_version{"mosaic-resource-profile-v0"};
    std::string scope{"desktop host proxy for process RSS, accelerator memory, and recurrent latency; not real 4GB mobile validation"};
    std::string device{"cpu"};
    std::optional<std::filesystem::path> checkpoint;
    Phase1Config model_config;
    ResourceProfileConfig resource_config;
    std::uint64_t parameter_count{};
    double runtime_weight_mib{};
    Phase1ProfileReport projected_weight_profile;
    ProcessMemoryProfile before_model, after_cpu_model, after_device_model, after_inference;
    std::vector<DepthLatencyProfile> depth_latency;
    bool finite_latency{};
    bool combined_host_proxy_within_budget{};
    double combined_peak_host_proxy_mib{};
    bool host_checks_passed{};
    std::string edge_verdict;
    [[nodiscard]] JsonValue::Object to_dict() const;
};

[[nodiscard]] ProcessMemoryProfile mosaic_process_memory() noexcept;
[[nodiscard]] double mosaic_nearest_rank_percentile(std::vector<double> values, double quantile);
[[nodiscard]] std::string resolve_mosaic_native_device(std::string_view device);
[[nodiscard]] ResourceProfileReport profile_mosaic_resources(
    const Phase1Config&, const ResourceProfileConfig&, std::string_view device = "auto",
    const std::optional<std::filesystem::path>& checkpoint = std::nullopt);

} // namespace swegca::world
