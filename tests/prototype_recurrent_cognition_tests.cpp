#include "checkpoint/prototype_checkpoint.hpp"
#include "swegca_architecture/sha256.hpp"
#include "world/prototype_recurrent_cognition.hpp"

#include <cassert>
#include <array>
#include <bit>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using swegca::architecture::Sha256;
using swegca::checkpoint::PrototypeCheckpoint;
using swegca::checkpoint::prototype_checkpoint_profiles;
using swegca::world::PrototypeRecurrentBundle;
using swegca::world::load_prototype_recurrent_bundle;

namespace {

std::string hex(const swegca::architecture::DigestBytes& digest) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result(digest.size() * 2, '0');
    for (std::size_t index = 0; index != digest.size(); ++index) {
        const auto value = std::to_integer<unsigned>(digest[index]);
        result[index * 2] = digits[value >> 4U];
        result[index * 2 + 1] = digits[value & 15U];
    }
    return result;
}

void append(Sha256& digest, const std::vector<float>& values, std::uint64_t& count) {
    digest.update(std::as_bytes(std::span(values)));
    count += values.size();
}

std::string recurrent_subset_sha256(const PrototypeRecurrentBundle& bundle,
                                    std::uint64_t& parameter_count) {
    Sha256 digest;
    const auto& w = bundle.weights;
    append(digest, w.role_embeddings, parameter_count);
    const std::span<const float> scale(&w.halt_evidence_scale, 1);
    digest.update(std::as_bytes(scale));
    ++parameter_count;
    append(digest, w.attention_norm_weight, parameter_count);
    append(digest, w.attention_norm_bias, parameter_count);
    append(digest, w.attention_in_projection_weight, parameter_count);
    append(digest, w.attention_in_projection_bias, parameter_count);
    append(digest, w.attention_out_projection_weight, parameter_count);
    append(digest, w.attention_out_projection_bias, parameter_count);
    append(digest, w.mlp_norm_weight, parameter_count);
    append(digest, w.mlp_norm_bias, parameter_count);
    append(digest, w.mlp_in_weight, parameter_count);
    append(digest, w.mlp_out_weight, parameter_count);
    append(digest, w.update_gate_weight, parameter_count);
    append(digest, w.update_gate_bias, parameter_count);
    append(digest, w.final_norm_weight, parameter_count);
    append(digest, w.final_norm_bias, parameter_count);
    append(digest, w.halt_head_weight, parameter_count);
    append(digest, w.halt_head_bias, parameter_count);
    return hex(digest.finish());
}

}  // namespace

int main() {
    static_assert(std::endian::native == std::endian::little,
                  "Prototype0 raw float subset fixture requires little endian");
    const auto profiles = prototype_checkpoint_profiles();
    assert(profiles.size() == 2);
    constexpr std::array<std::string_view, 2> expected_subset_sha{
        "01883a5a0f59f03f99d54a24f59c7f8fb6eda1318c89bee7779b5af6c7830e9b",
        "62caada03d4e618804cc3058714d6a99a9fabf08a5d50edfc7e6b7bf8976e91d"};
    for (std::size_t index = 0; index != profiles.size(); ++index) {
        const auto& profile = profiles[index];
        assert(std::filesystem::is_regular_file(profile.original_path));
        auto checkpoint = PrototypeCheckpoint::open(profile.original_path);
        auto bundle = load_prototype_recurrent_bundle(checkpoint);

        assert(bundle.checkpoint_sha256 == profile.checkpoint_sha256);
        assert(bundle.recurrent_source_sha256 ==
               "5ea32f8d6b97ae8db013e09b1cb4d42f49e9c1cd1794d8d8f5bad40ea519ef6e");
        assert(bundle.source_config_sha256 ==
               "543784817730b6e79261621d8b7b4ce3d54eff23a9027788d5104e05cdd7eea5");
        assert(bundle.runtime_config_canonical_sha256 ==
               "d5d58abcc1e13c31d501231b22e8ac19f956d6d1f98594e9ced0e8dd342bf301");
        assert(bundle.config.state.semantic_slots == 32);
        assert(bundle.config.state.executive_slots == 8);
        assert(bundle.config.state.scratch_slots == 8);
        assert(bundle.config.state.hidden_dim == 2048);
        assert(bundle.config.attention_heads == 16);
        assert(bundle.config.mlp_hidden_dim == 16384);
        assert(bundle.config.minimum_cycles == 1 && bundle.config.maximum_cycles == 4);
        assert(bundle.config.halt_threshold == 0.5F);
        assert(bundle.config.evidence_logit_epsilon == 1.0e-4F);
        assert(bundle.config.maximum_update == 1.0F);

        std::uint64_t parameter_count = 0;
        const auto subset_sha = recurrent_subset_sha256(bundle, parameter_count);
        assert(parameter_count == 117'471'235ULL);
        assert(subset_sha == expected_subset_sha[index]);
        std::cout << "Prototype0 recurrent bundle passed checkpoint="
                  << bundle.checkpoint_sha256 << " parameters=" << parameter_count
                  << " sha256=" << subset_sha << '\n';
    }
}
