#include "world/prototype_recurrent_cognition.hpp"

#include "checkpoint/prototype_materialized_tensor.hpp"

#include <stdexcept>
#include <string_view>

namespace swegca::world {
namespace {

constexpr std::string_view recurrent_source_sha256 =
    "5ea32f8d6b97ae8db013e09b1cb4d42f49e9c1cd1794d8d8f5bad40ea519ef6e";

[[nodiscard]] std::vector<float> tensor_values(
    const checkpoint::PrototypeCheckpoint& checkpoint, const std::string_view key) {
    auto tensor = checkpoint::materialize_prototype_tensor(checkpoint, key);
    if (tensor.dtype() != checkpoint::TensorDType::float32) {
        throw std::runtime_error("Prototype0 recurrent tensor is not float32");
    }
    return tensor.float32_values();
}

}  // namespace

PrototypeRecurrentBundle load_prototype_recurrent_bundle(
    const checkpoint::PrototypeCheckpoint& checkpoint) {
    if (checkpoint.manifest().profile == nullptr ||
        checkpoint.manifest().checkpoint_sha256.empty()) {
        throw std::invalid_argument("Prototype0 recurrent load requires an audited checkpoint");
    }
    const auto& profile = *checkpoint.manifest().profile;
    const auto& source = profile.runtime_cognition_config;
    PrototypeRecurrentBundle bundle;
    bundle.config = {
        {source.semantic_slots, source.executive_slots, source.scratch_slots,
         source.hidden_dim},
        source.attention_heads, source.mlp_hidden_dim, source.minimum_cycles,
        source.maximum_cycles, static_cast<float>(source.halt_threshold),
        static_cast<float>(source.evidence_logit_epsilon),
        static_cast<float>(source.maximum_update)};
    bundle.checkpoint_sha256 = checkpoint.manifest().checkpoint_sha256;
    bundle.recurrent_source_sha256 = std::string(recurrent_source_sha256);
    bundle.source_config_sha256 = std::string(profile.source_config_sha256);
    bundle.runtime_config_canonical_sha256 =
        std::string(profile.runtime_cognition_config_canonical_sha256);

    auto& weights = bundle.weights;
    weights.role_embeddings = tensor_values(checkpoint, "cognition.role_embeddings");
    const auto halt_scale = tensor_values(checkpoint, "cognition.halt_evidence_scale");
    if (halt_scale.size() != 1) {
        throw std::runtime_error("Prototype0 halt evidence scale is not scalar");
    }
    weights.halt_evidence_scale = halt_scale.front();
    weights.attention_norm_weight = tensor_values(
        checkpoint, "cognition.cell.attention_norm.weight");
    weights.attention_norm_bias = tensor_values(
        checkpoint, "cognition.cell.attention_norm.bias");
    weights.attention_in_projection_weight = tensor_values(
        checkpoint, "cognition.cell.attention.in_proj_weight");
    weights.attention_in_projection_bias = tensor_values(
        checkpoint, "cognition.cell.attention.in_proj_bias");
    weights.attention_out_projection_weight = tensor_values(
        checkpoint, "cognition.cell.attention.out_proj.weight");
    weights.attention_out_projection_bias = tensor_values(
        checkpoint, "cognition.cell.attention.out_proj.bias");
    weights.mlp_norm_weight = tensor_values(checkpoint, "cognition.cell.mlp_norm.weight");
    weights.mlp_norm_bias = tensor_values(checkpoint, "cognition.cell.mlp_norm.bias");
    weights.mlp_in_weight = tensor_values(checkpoint, "cognition.cell.mlp_in.weight");
    weights.mlp_out_weight = tensor_values(checkpoint, "cognition.cell.mlp_out.weight");
    weights.update_gate_weight = tensor_values(
        checkpoint, "cognition.cell.update_gate.weight");
    weights.update_gate_bias = tensor_values(
        checkpoint, "cognition.cell.update_gate.bias");
    weights.final_norm_weight = tensor_values(checkpoint, "cognition.final_norm.weight");
    weights.final_norm_bias = tensor_values(checkpoint, "cognition.final_norm.bias");
    weights.halt_head_weight = tensor_values(checkpoint, "cognition.halt_head.weight");
    weights.halt_head_bias = tensor_values(checkpoint, "cognition.halt_head.bias");
    weights.validate(bundle.config);
    return bundle;
}

}  // namespace swegca::world
