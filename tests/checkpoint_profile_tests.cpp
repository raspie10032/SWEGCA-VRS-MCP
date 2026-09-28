#include "checkpoint/checkpoint_profile.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>

namespace {

void check(const bool condition, const char* expression, const int line) {
    if (!condition) {
        std::cerr << "CHECK failed at line " << line << ": " << expression << '\n';
        std::exit(1);
    }
}

#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

using swegca::checkpoint::CheckpointProfile;
using swegca::checkpoint::DTypeProfile;
using swegca::checkpoint::TensorDType;
using swegca::checkpoint::TensorLayout;
using swegca::checkpoint::TensorLayoutError;

const DTypeProfile* dtype(const CheckpointProfile& profile, const TensorDType wanted) {
    for (const auto& item : profile.dtype_profile) {
        if (item.dtype == wanted) {
            return &item;
        }
    }
    return nullptr;
}

void check_identity_profiles() {
    const auto profiles = swegca::checkpoint::checkpoint_profiles();
    CHECK(profiles.size() == 3);

    const auto& narrative = profiles[0];
    CHECK(narrative.file_name == "mosaic_unified_narrative_evidence.pt");
    CHECK(narrative.checkpoint_sha256 ==
          "041dfd03d9b89608ef093e31b95b7527f8151b794b4d3ffdd45fb45d4bf46847");
    CHECK(narrative.checkpoint_bytes == 536734499ULL);
    CHECK(narrative.model_config_canonical_sha256 ==
          "fb5e469ec8a630335b4058c31f8e869cac7bf668841a3d7814af552a2a03ea3e");
    CHECK(narrative.model_tensor_count == 290);
    CHECK(narrative.all_tensor_count == 291);
    CHECK(narrative.model_numel == 134084034ULL);
    CHECK(dtype(narrative, TensorDType::float16)->tensor_count == 1);
    CHECK(dtype(narrative, TensorDType::float16)->element_count == 131072ULL);
    CHECK(dtype(narrative, TensorDType::float32)->tensor_count == 290);
    CHECK(dtype(narrative, TensorDType::float32)->element_count == 134084034ULL);
    CHECK(dtype(narrative, TensorDType::bfloat16) == nullptr);

    const auto& cognition = profiles[1];
    CHECK(cognition.file_name == "rozephine_single_state.pt");
    CHECK(cognition.checkpoint_sha256 ==
          "56e25cf3cb60bbffe4351efca80cfb79abb9e858fb32d9931273de509c4715eb");
    CHECK(cognition.checkpoint_bytes == 535171130ULL);
    CHECK(cognition.model_config_canonical_sha256 ==
          "452c6151b14392aba08aab317677c9128d9e68abb90f89a7774905c203f8c8e0");
    CHECK(cognition.model_tensor_count == 356);
    CHECK(cognition.all_tensor_count == 357);
    CHECK(cognition.model_numel == 135516698ULL);
    CHECK(dtype(cognition, TensorDType::float32)->tensor_count == 259);
    CHECK(dtype(cognition, TensorDType::float32)->element_count == 131859648ULL);
    CHECK(dtype(cognition, TensorDType::bfloat16)->tensor_count == 97);
    CHECK(dtype(cognition, TensorDType::bfloat16)->element_count == 3657050ULL);

    const auto& synapse = profiles[2];
    CHECK(synapse.file_name == "rozephine_single_state_synapse.pt");
    CHECK(synapse.checkpoint_sha256 ==
          "f922cea14b9cd1061ab5c24a5f54025948c288ee24a6a746577efbf14d5c1b9b");
    CHECK(synapse.checkpoint_bytes == 535479950ULL);
    CHECK(synapse.model_config_canonical_sha256 ==
          "d08834ca25fd4bb67b1c4697561e1cf593922b6abfa016f99cab62732d5f2cc4");
    CHECK(synapse.model_tensor_count == 368);
    CHECK(synapse.all_tensor_count == 369);
    CHECK(synapse.model_numel == 135592093ULL);
    CHECK(dtype(synapse, TensorDType::float32)->tensor_count == 270);
    CHECK(dtype(synapse, TensorDType::float32)->element_count == 131935042ULL);
    CHECK(dtype(synapse, TensorDType::int64)->tensor_count == 1);
    CHECK(dtype(synapse, TensorDType::int64)->element_count == 1);

    for (const auto& profile : profiles) {
        CHECK(swegca::checkpoint::validate_checkpoint_profile(profile));
        CHECK(swegca::checkpoint::find_checkpoint_profile(profile.checkpoint_sha256) == &profile);
    }
    CHECK(swegca::checkpoint::find_checkpoint_profile(
              "0000000000000000000000000000000000000000000000000000000000000000") ==
          nullptr);
}

void check_exact_key_sets() {
    const auto profiles = swegca::checkpoint::checkpoint_profiles();
    const auto& narrative = profiles[0];
    const auto& cognition = profiles[1];
    const auto& synapse = profiles[2];

    CHECK(narrative.model_state_keys.size() == 290);
    CHECK(cognition.model_state_keys.size() == 356);
    CHECK(synapse.model_state_keys.size() == 368);
    for (std::size_t index = 0; index < narrative.model_state_keys.size(); ++index) {
        CHECK(narrative.model_state_keys[index] == cognition.model_state_keys[index]);
        CHECK(narrative.model_state_keys[index] == synapse.model_state_keys[index]);
    }
    for (std::size_t index = 0; index < cognition.model_state_keys.size(); ++index) {
        CHECK(cognition.model_state_keys[index] == synapse.model_state_keys[index]);
    }
    CHECK(synapse.model_state_keys.front() == "text_core.workspace");
    CHECK(narrative.model_state_keys.back() == "audio_teacher_projection.bias");
    CHECK(cognition.model_state_keys[290] == "video_object_tracker.queries");
    CHECK(cognition.model_state_keys.back() == "video_egomotion_head.3.bias");
    CHECK(synapse.model_state_keys[356] == "cross_modal_text_projection.weight");
    CHECK(synapse.model_state_keys.back() == "cross_modal_evidence_head.1.bias");
    CHECK(!swegca::checkpoint::contains_model_key(narrative, "video_object_tracker.queries"));
    CHECK(!swegca::checkpoint::contains_model_key(cognition,
                                                  "cross_modal_text_projection.weight"));
    CHECK(swegca::checkpoint::contains_model_key(synapse,
                                                 "cross_modal_text_projection.weight"));
}

void check_teacher_projection() {
    for (const auto& profile : swegca::checkpoint::checkpoint_profiles()) {
        const auto& layout = profile.fixed_teacher_projection;
        CHECK(layout.dtype == TensorDType::float16);
        CHECK(layout.shape.size() == 2);
        CHECK(layout.shape[0] == 512);
        CHECK(layout.shape[1] == 256);
        CHECK(layout.stride.size() == 2);
        CHECK(layout.stride[0] == 1);
        CHECK(layout.stride[1] == 512);
        CHECK(layout.storage_offset_elements == 0);
        CHECK(layout.storage_elements == 131072);

        const auto checked = swegca::checkpoint::check_tensor_span(layout);
        CHECK(checked);
        CHECK(checked.numel == 131072);
        CHECK(checked.logical_bytes == 262144);
        CHECK(checked.storage_span_start_byte == 0);
        CHECK(checked.storage_span_end_byte_exclusive == 262144);
        CHECK(checked.storage_span_bytes == 262144);
        CHECK(!checked.contiguous);
    }
}

void check_layout_failures() {
    constexpr std::array<std::uint64_t, 2> shape{2, 3};
    constexpr std::array<std::int64_t, 2> contiguous_stride{3, 1};
    auto checked = swegca::checkpoint::check_tensor_span(
        TensorLayout{TensorDType::float32, shape, contiguous_stride, 4, 10});
    CHECK(checked);
    CHECK(checked.numel == 6);
    CHECK(checked.logical_bytes == 24);
    CHECK(checked.storage_span_start_byte == 16);
    CHECK(checked.storage_span_end_byte_exclusive == 40);
    CHECK(checked.contiguous);

    constexpr std::array<std::int64_t, 1> short_stride{1};
    CHECK(swegca::checkpoint::check_tensor_span(
              TensorLayout{TensorDType::float32, shape, short_stride, 0, 6})
              .error == TensorLayoutError::rank_mismatch);

    constexpr std::array<std::int64_t, 2> negative_stride{3, -1};
    CHECK(swegca::checkpoint::check_tensor_span(
              TensorLayout{TensorDType::float32, shape, negative_stride, 0, 6})
              .error == TensorLayoutError::negative_stride);

    CHECK(swegca::checkpoint::check_tensor_span(
              TensorLayout{TensorDType::float32, shape, contiguous_stride, 5, 10})
              .error == TensorLayoutError::storage_out_of_bounds);

    constexpr std::array<std::uint64_t, 2> overflow_shape{
        std::numeric_limits<std::uint64_t>::max(), 2};
    CHECK(swegca::checkpoint::check_tensor_span(
              TensorLayout{TensorDType::float32, overflow_shape, contiguous_stride, 0,
                           std::numeric_limits<std::uint64_t>::max()})
              .error == TensorLayoutError::arithmetic_overflow);
}

}  // namespace

int main() {
    check_identity_profiles();
    check_exact_key_sets();
    check_teacher_projection();
    check_layout_failures();
    std::cout << "checkpoint profile tests passed\n";
}
