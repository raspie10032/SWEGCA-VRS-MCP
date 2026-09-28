#include "checkpoint/prototype_checkpoint.hpp"
#include "swegca_architecture/sha256.hpp"

#include <array>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace swegca::checkpoint;

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

std::string file_sha256(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    assert(input);
    swegca::architecture::Sha256 digest;
    std::array<char, 64U << 10> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0) {
            digest.update(std::as_bytes(
                std::span(buffer.data(), static_cast<std::size_t>(count))));
        }
    }
    assert(input.eof());
    return hex(digest.finish());
}

}  // namespace

int main() {
    const auto profiles = prototype_checkpoint_profiles();
    assert(profiles.size() == 2);
    assert(find_prototype_checkpoint_profile("not-an-audited-digest") == nullptr);

    for (const auto& profile : profiles) {
        assert(std::filesystem::is_regular_file(profile.original_path));
        assert(profile.checkpoint_bytes == 529'029'365ULL);
        assert(profile.data_pickle_bytes == 6047);
        assert(profile.pickle_opcode_count == 1499);
        assert(profile.tensor_manifest_sha256 ==
               "4a67cf5320921a1a247d96d19a79efff4835e6448708403ebe78a2af9e95bd3c");
        assert(profile.tensors.size() == 38);
        assert(profile.members.size() == 44);
        assert(std::filesystem::is_regular_file(profile.source_config_path));
        assert(file_sha256(profile.source_config_path) == profile.source_config_sha256);
        assert(profile.source_config_sha256 ==
               "543784817730b6e79261621d8b7b4ce3d54eff23a9027788d5104e05cdd7eea5");
        assert(profile.runtime_cognition_config_canonical_sha256 ==
               "d5d58abcc1e13c31d501231b22e8ac19f956d6d1f98594e9ced0e8dd342bf301");
        const auto& config = profile.runtime_cognition_config;
        assert(config.semantic_slots == 32 && config.executive_slots == 8 &&
               config.scratch_slots == 8 && config.hidden_dim == 2048);
        assert(config.attention_heads == 16 && config.mlp_hidden_dim == 16384);
        assert(config.minimum_cycles == 1 && config.maximum_cycles == 4);
        assert(config.halt_threshold == 0.5 && config.evidence_logit_epsilon == 1e-4 &&
               config.maximum_update == 1.0);

        bool legacy_gate_rejected = false;
        try {
            (void)RestrictedCheckpoint::open(profile.original_path);
        } catch (const std::runtime_error&) {
            legacy_gate_rejected = true;
        }
        assert(legacy_gate_rejected);

        auto checkpoint = PrototypeCheckpoint::open(profile.original_path);
        const auto& manifest = checkpoint.manifest();
        assert(manifest.profile == &profile);
        assert(manifest.archive_root == "prototype0");
        assert(manifest.checkpoint_sha256 == profile.checkpoint_sha256);
        assert(manifest.data_pickle_sha256 == profile.data_pickle_sha256);
        assert(manifest.serialization_id == profile.serialization_id);
        assert(manifest.tensor_manifest_sha256 == profile.tensor_manifest_sha256);
        assert(manifest.pickle_opcode_count == profile.pickle_opcode_count);
        assert(manifest.tensors.size() == 38);

        const auto* first = manifest.find_tensor("bridges.text.projection.0.weight");
        assert(first != nullptr && first->storage_key == "0");
        assert(first->dtype == TensorDType::float32 && first->contiguous);
        assert((first->shape == std::vector<std::uint64_t>{2048, 8}));
        assert((first->stride == std::vector<std::int64_t>{8, 1}));
        assert(first->logical_bytes == 65'536);

        const auto* scalar = manifest.find_tensor("cognition.halt_evidence_scale");
        assert(scalar != nullptr && scalar->storage_key == "17");
        assert(scalar->shape.empty() && scalar->stride.empty());
        assert(scalar->numel == 1 && scalar->logical_bytes == 4 && scalar->contiguous);

        const auto* last = manifest.find_tensor("image_geometry_head.bias");
        assert(last != nullptr && last->storage_key == "37");
        assert((last->shape == std::vector<std::uint64_t>{4}));
        assert(checkpoint.read_storage("37").size() == 16);
        assert(manifest.find_tensor("not.a.real.tensor") == nullptr);

        bool missing_rejected = false;
        try {
            (void)checkpoint.read_storage("not-a-storage-key");
        } catch (const std::runtime_error&) {
            missing_rejected = true;
        }
        assert(missing_rejected);
        std::cout << "PASS " << profile.checkpoint_sha256 << " tensors="
                  << manifest.tensors.size() << " opcodes="
                  << manifest.pickle_opcode_count << '\n';
    }
}
