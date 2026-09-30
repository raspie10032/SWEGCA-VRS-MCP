#pragma once

#include "world/cognitive_event.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view cognitive_kernel_source_sha256 =
    "7f40002114efc8fbb71865628aaa6a0a66be176208c6410d42370a8f5a0c586e";
inline constexpr std::uint64_t default_deployment_budget_bytes = 8ULL * 1024 * 1024 * 1024;

struct RepresentationRegistryEntry final {
    std::string id;
    std::string type;
    JsonValue::Array native_shape;
    std::string encoder;
    std::optional<std::string> decoder;
    std::string bridge_in;
    std::optional<std::string> bridge_out;
    JsonValue::Object device_requirements;
    std::string precision;
    std::string license;
    std::vector<std::string> capabilities;

    RepresentationRegistryEntry(std::string id, std::string type,
        JsonValue::Array native_shape, std::string encoder,
        std::optional<std::string> decoder, std::string bridge_in,
        std::optional<std::string> bridge_out, JsonValue::Object device_requirements,
        std::string precision, std::string license,
        std::vector<std::string> capabilities);
    [[nodiscard]] JsonValue to_dict() const;
    [[nodiscard]] static RepresentationRegistryEntry from_dict(const JsonValue& value);
};

struct PhysicalQuery final {
    std::string query_id;
    std::string query_type;
    JsonValue::Object origin;
    std::string scene_ref;
    JsonValue::Object requirements;

    PhysicalQuery(std::string query_id, std::string query_type,
        JsonValue::Object origin, std::string scene_ref,
        JsonValue::Object requirements);
    [[nodiscard]] JsonValue to_dict() const;
    [[nodiscard]] static PhysicalQuery from_dict(const JsonValue& value);
};

struct PhysicalEvidence final {
    std::string event_id;
    std::string query_id;
    std::string evidence_type;
    std::vector<EvidenceClaim> claims;
    std::string backend;
    std::string scene_ref;
    bool deterministic{};

    PhysicalEvidence(std::string event_id, std::string query_id,
        std::string evidence_type, std::vector<EvidenceClaim> claims,
        std::string backend, std::string scene_ref, bool deterministic);
    [[nodiscard]] JsonValue to_dict() const;
    [[nodiscard]] static PhysicalEvidence from_dict(const JsonValue& value);
};

struct WorldBundle final {
    std::string bundle_id;
    std::vector<std::string> entity_ids;
    std::map<std::string, std::vector<std::string>, std::less<>> representations;
    JsonValue::Object entity_relation_metadata;
    JsonValue::Object provenance;
    std::string license;

    WorldBundle(std::string bundle_id, std::vector<std::string> entity_ids,
        std::map<std::string, std::vector<std::string>, std::less<>> representations,
        JsonValue::Object entity_relation_metadata, JsonValue::Object provenance,
        std::string license);
    [[nodiscard]] JsonValue to_dict() const;
    [[nodiscard]] static WorldBundle from_dict(const JsonValue& value);
};

struct RecurrentCoreSpec final {
    std::uint64_t hidden_dim{2048};
    std::uint64_t unique_blocks{16};
    std::uint64_t attention_heads{16};
    std::uint64_t kv_heads{4};
    std::uint64_t mlp_hidden_dim{5504};
    std::uint64_t max_cycles{8};
    CognitiveKernelConfig state{};
    void validate() const;
};

struct VramEstimateConfig final {
    double weight_bits{4.0};
    std::uint64_t activation_bytes{2};
    std::uint64_t batch_size{1};
    double activation_multiplier{4.0};
    std::uint64_t bridge_bytes{512ULL * 1024 * 1024};
    std::uint64_t specialist_bytes{512ULL * 1024 * 1024};
    std::uint64_t runtime_bytes{1024ULL * 1024 * 1024};
    std::uint64_t reserve_bytes{1024ULL * 1024 * 1024};
    std::uint64_t budget_bytes{default_deployment_budget_bytes};
    void validate() const;
};

struct RecurrentCoreParameterEstimate final {
    std::uint64_t attention_per_block{};
    std::uint64_t mlp_per_block{};
    std::uint64_t norm_per_block{};
    std::uint64_t block_parameters{};
    std::uint64_t state_embeddings{};
    std::uint64_t final_norm{};
    std::uint64_t total_parameters{};
};

struct InferenceVramEstimate final {
    std::uint64_t parameters{};
    std::uint64_t weight_bytes{};
    std::uint64_t state_bytes{};
    std::uint64_t kv_bytes{};
    std::uint64_t activation_staging_bytes{};
    std::uint64_t bridge_bytes{};
    std::uint64_t specialist_bytes{};
    std::uint64_t runtime_bytes{};
    std::uint64_t reserve_bytes{};
    std::uint64_t total_bytes{};
    std::uint64_t budget_bytes{};
    std::int64_t headroom_bytes{};
    bool fits_budget{};
    std::uint64_t max_cycles{};
};

[[nodiscard]] RecurrentCoreParameterEstimate estimate_recurrent_core_parameters(
    const RecurrentCoreSpec& spec);
[[nodiscard]] InferenceVramEstimate estimate_inference_vram(
    const RecurrentCoreSpec& spec,
    const VramEstimateConfig& config = {});

}  // namespace swegca::world
