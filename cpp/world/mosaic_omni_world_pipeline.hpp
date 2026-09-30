#pragma once

#include "world/modal_to_world.hpp"
#include "world/mosaic_omni_narrative.hpp"
#include "world/mosaic_omni_visual.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view mosaic_omni_world_pipeline_source_sha256 =
    "a06f7a85128827b036a7c1801920e570e187c0163df6c3851d85ef2dd3bb5e20";

struct WorldPipelineLinear final { std::vector<double> weight, bias; };
struct WorldPipelineCellWeights final {
    std::vector<double> norm1_weight, norm1_bias;
    std::vector<double> attention_in_weight, attention_in_bias;
    std::vector<double> attention_out_weight, attention_out_bias;
    std::vector<double> norm2_weight, norm2_bias;
    WorldPipelineLinear feedforward_in, feedforward_out;
};
struct MosaicOmniWorldPipelineConfig final {
    WorldConfig world;
    std::size_t attention_heads{}, world_ffn_dim{}, world_rounds{}, object_slots{};
    double video_temporal_delta_scale{1.0};
    bool audio_temporal_head_available{};
    bool video_pair_trajectory_binding_available{};
    bool video_descriptor_trajectory_binding_available{};
    void validate() const;
};
struct MosaicOmniWorldPipelineWeights final {
    std::optional<WorldPipelineLinear> video_object_to_world;
    std::optional<WorldPipelineLinear> video_temporal_to_world;
    std::optional<WorldPipelineLinear> video_temporal_delta_to_world;
    std::optional<WorldPipelineLinear> audio_event_slot_projection;
    std::optional<WorldPipelineLinear> audio_content_to_world;
    WorldPipelineCellWeights world_cell;
};
struct MosaicOmniWorldPipelineModules final {
    const ModalToWorldAdapter* to_world{};
    const VisualTeacherSlotBridge* visual_teacher_slot_bridge{};
    const ExplicitObjectRelationGrounder* relation_grounder{};
    const ExplicitRelationHead* relation_head{};
};
struct MosaicOmniWorldPipelineInputs final {
    std::vector<Tensor> source_tokens;
    std::vector<BooleanMask> source_masks;
    std::vector<std::string> modalities;
    std::optional<Tensor> camera_pose_states;
    std::optional<Tensor> action_summary;
    Tensor text_world;
    std::optional<BooleanMask> subject_descriptor_mask, object_descriptor_mask;
    std::optional<MosaicTokenBatch> subject_descriptor_input_ids, object_descriptor_input_ids;
    const MosaicTextLM* descriptor_text_core{};
    const NarrativeDescriptorProjection* descriptor_projection{};
    std::optional<Tensor> image_patch_tokens;
    std::optional<std::pair<std::size_t, std::size_t>> image_patch_grid;
    std::optional<Tensor> video_object_slots;
    std::optional<Tensor> video_temporal_summary;
    std::optional<Tensor> video_temporal_delta;
    std::optional<Tensor> audio_world_summary;
    std::optional<Tensor> audio_temporal_features;
    std::optional<Tensor> audio_content_features;
};
struct MosaicOmniWorldPipelineOutput final {
    WorldState world_state;
    std::optional<std::pair<Tensor, Tensor>> descriptor_queries;
    std::optional<Tensor> explicit_relation_logits, explicit_object_attention;
    std::optional<Tensor> video_world_summary, video_temporal_delta_world;
    std::optional<Tensor> audio_event_slots, audio_content_event_slots;
};

class MosaicOmniWorldPipeline final {
public:
    MosaicOmniWorldPipeline(MosaicOmniWorldPipelineConfig,
        MosaicOmniWorldPipelineWeights, MosaicOmniWorldPipelineModules);
    [[nodiscard]] MosaicOmniWorldPipelineOutput forward(
        const MosaicOmniWorldPipelineInputs&) const;
private:
    MosaicOmniWorldPipelineConfig config_;
    MosaicOmniWorldPipelineWeights weights_;
    MosaicOmniWorldPipelineModules modules_;
};

} // namespace swegca::world
