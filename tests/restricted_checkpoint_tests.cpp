#include "checkpoint/restricted_checkpoint.hpp"

#include <cassert>
#include <filesystem>
#include <iostream>
#include <stdexcept>

using namespace swegca::checkpoint;

int main() {
    std::uint64_t tensor_total = 0;
    std::uint64_t byte_total = 0;
    for (const auto& profile : checkpoint_profiles()) {
        assert(std::filesystem::is_regular_file(profile.original_path));
        auto checkpoint = RestrictedCheckpoint::open(profile.original_path);
        const auto& manifest = checkpoint.manifest();
        assert(manifest.profile == &profile);
        assert(manifest.checkpoint_sha256 == profile.checkpoint_sha256);
        assert(manifest.data_pickle_sha256 == profile.data_pickle_sha256);
        assert(manifest.model_config_canonical_sha256 == profile.model_config_canonical_sha256);
        assert(manifest.serialization_id == profile.serialization_id);
        assert(manifest.tensors.size() == profile.all_tensor_count);
        assert(manifest.model_tensors().size() == profile.model_tensor_count);
        assert(manifest.pickle_opcode_count > 10'000);

        const auto& teacher = manifest.tensors.front();
        assert(!teacher.model_state);
        assert(teacher.state_path == "visual_pretraining.fixed_teacher_projection");
        assert(teacher.dtype == TensorDType::float16);
        assert((teacher.shape == std::vector<std::uint64_t>{512, 256}));
        assert((teacher.stride == std::vector<std::int64_t>{1, 512}));
        assert(!teacher.contiguous && teacher.logical_bytes == 262144);
        assert(checkpoint.read_storage(teacher.storage_key).size() == 262144);

        assert(manifest.find_model_tensor(profile.model_state_keys.front()) != nullptr);
        assert(manifest.find_model_tensor(profile.model_state_keys.back()) != nullptr);
        assert(manifest.find_model_tensor("not.a.real.tensor") == nullptr);
        bool missing_rejected = false;
        try {
            (void)checkpoint.read_storage("not-a-storage-key");
        } catch (const std::out_of_range&) {
            missing_rejected = true;
        }
        assert(missing_rejected);
        tensor_total += manifest.tensors.size();
        byte_total += profile.checkpoint_bytes;
        std::cout << "PASS " << profile.file_name << " tensors=" << manifest.tensors.size()
                  << " opcodes=" << manifest.pickle_opcode_count << '\n';
    }
    assert(tensor_total == 1017);
    assert(byte_total == 1'607'385'579ULL);
    std::cout << "PASS restricted checkpoint gate tensors=" << tensor_total
              << " bytes=" << byte_total << '\n';
}
