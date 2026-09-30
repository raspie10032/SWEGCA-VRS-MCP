#pragma once

#include "world/language_generation.hpp"
#include "world/mosaic_omni.hpp"
#include "world/mosaic_omni_narrative.hpp"

#include <functional>
#include <filesystem>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace swegca::world {

struct MosaicUnifiedWeights final {
    MosaicTextWeights text;
    std::map<std::string, Tensor, std::less<>> parameters;
};

struct MosaicUnifiedInput final {
    MosaicTokenBatch input_ids;
    std::optional<MosaicTokenBatch> world_input_ids;
    std::optional<Tensor> pixel_values;
    std::optional<BooleanMask> subject_descriptor_mask;
    std::optional<BooleanMask> object_descriptor_mask;
    std::optional<MosaicTokenBatch> subject_descriptor_input_ids;
    std::optional<MosaicTokenBatch> object_descriptor_input_ids;
    std::optional<Tensor> audio_values;
    std::optional<Tensor> video_values;
    std::optional<Tensor> camera_pose_values;
    std::optional<Tensor> action_values;
    std::optional<MosaicTokenBatch> targets;
    std::optional<MosaicTokenBatch> question_input_ids;
    std::optional<std::vector<std::int64_t>> answerability_labels;
    std::optional<std::size_t> text_rounds;
};

struct MosaicUnifiedGenerateInput final {
    MosaicTokenBatch input_ids;
    std::int64_t maximum_new_bytes{};
    std::optional<MosaicTokenBatch> world_input_ids;
    std::optional<Tensor> pixel_values;
    std::optional<Tensor> audio_values;
    std::optional<Tensor> video_values;
    std::optional<std::size_t> text_rounds;
    bool use_answerability_gate{};
    std::vector<DetachedProposalRequest> language_requests;
    std::function<std::string()> language_snapshot_id;
};

class MosaicUnifiedForConditionalGeneration final {
public:
    MosaicUnifiedForConditionalGeneration(
        MosaicUnifiedConfig config, MosaicUnifiedWeights weights);
    ~MosaicUnifiedForConditionalGeneration();
    MosaicUnifiedForConditionalGeneration(MosaicUnifiedForConditionalGeneration&&) noexcept;
    MosaicUnifiedForConditionalGeneration& operator=(MosaicUnifiedForConditionalGeneration&&) noexcept;
    MosaicUnifiedForConditionalGeneration(const MosaicUnifiedForConditionalGeneration&) = delete;
    MosaicUnifiedForConditionalGeneration& operator=(const MosaicUnifiedForConditionalGeneration&) = delete;

    [[nodiscard]] const MosaicUnifiedConfig& config() const noexcept;
    [[nodiscard]] MosaicUnifiedOutput forward(const MosaicUnifiedInput& input) const;
    [[nodiscard]] MosaicTokenBatch generate(const MosaicUnifiedGenerateInput& input) const;
    [[nodiscard]] Tensor encode_visual_summary(const Tensor& pixel_values) const;
    [[nodiscard]] Tensor encode_visual_regions(
        const Tensor& pixel_values, const Tensor& boxes_xyxy_normalized) const;
    [[nodiscard]] Tensor encode_visual_region_grids(
        const Tensor& pixel_values, const Tensor& boxes_xyxy_normalized,
        std::size_t grid_size = 2) const;
    [[nodiscard]] NarrativeContinuityOutput score_narrative_continuity(
        const MosaicTokenBatch& first, const MosaicTokenBatch& second) const;
    [[nodiscard]] LongVideoWorldOutput accumulate_long_video(
        const Tensor& clip_world_states, const BooleanMask& clip_mask,
        const WorldState* initial_state = nullptr) const;
    void configure_language_backend(
        const std::optional<std::filesystem::path>& config_path,
        const std::optional<std::string>& profile = std::nullopt);
    void configure_language_registry(
        std::shared_ptr<const class ProviderRegistry> registry,
        std::string profile);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace swegca::world
