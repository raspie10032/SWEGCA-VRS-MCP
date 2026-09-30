#include "world/mosaic_unified.hpp"
#include "world/agent_interface.hpp"
#include "world/mosaic_omni_cross_modal.hpp"
#include "world/mosaic_omni_modal_frontends.hpp"
#include "world/mosaic_omni_output.hpp"
#include "world/mosaic_omni_text_pipeline.hpp"
#include "world/mosaic_omni_video_pipeline.hpp"
#include "world/mosaic_omni_world_pipeline.hpp"
#include "world/mosaic_unified_materialization.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <ranges>
#include <span>
#include <stdexcept>
#include <utility>

namespace swegca::world {
namespace {

void rectangular(const MosaicTokenBatch& rows, const char* name) {
    if (rows.empty() || rows.front().empty())
        throw std::invalid_argument(std::string(name) + " must be nonempty");
    for (const auto& row : rows) if (row.size() != rows.front().size())
        throw std::invalid_argument(std::string(name) + " must be rectangular");
}

void validate_bos_batch(const MosaicTokenBatch& rows, const char* name,
                        const std::optional<std::size_t> batch = std::nullopt) {
    rectangular(rows, name);
    if (batch && rows.size() != *batch)
        throw std::invalid_argument(std::string(name) + " batch mismatch");
    for (const auto& row : rows) if (row.front() != mosaic_bos_id)
        throw std::invalid_argument(std::string(name) + " must begin with BOS_ID");
}

Tensor linear_boundary(const Tensor& input, const ModalLinearWeights& weights,
                       const std::size_t output_width) {
    const auto shape = input.shape();
    if (shape.empty()) throw std::invalid_argument("unified boundary linear input has no rank");
    const auto input_width = static_cast<std::size_t>(shape.back());
    if (weights.weight.size() != input_width * output_width ||
        (!weights.bias.empty() && weights.bias.size() != output_width))
        throw std::invalid_argument("unified boundary linear weight shape mismatch");
    const auto rows = input.values().size() / input_width;
    std::vector<double> values(rows * output_width);
    for (std::size_t row = 0; row < rows; ++row)
        for (std::size_t out = 0; out < output_width; ++out) {
            double value = weights.bias.empty() ? 0.0 : weights.bias[out];
            for (std::size_t in = 0; in < input_width; ++in)
                value += input.values()[row * input_width + in] *
                         weights.weight[out * input_width + in];
            values[row * output_width + out] = value;
        }
    std::vector<std::uint64_t> result_shape(shape.begin(), shape.end());
    result_shape.back() = output_width;
    return Tensor(input.dtype(), std::move(result_shape), std::move(values),
                  std::string(input.device()));
}

Tensor layer_norm_boundary(const Tensor& input, std::span<const double> weight,
                           std::span<const double> bias) {
    const auto shape = input.shape();
    if (shape.empty()) throw std::invalid_argument("unified World norm input has no rank");
    const auto width = static_cast<std::size_t>(shape.back());
    if (weight.size() != width || bias.size() != width)
        throw std::invalid_argument("unified World norm weight shape mismatch");
    const auto rows = input.values().size() / width;
    std::vector<double> values(input.values().size());
    for (std::size_t row = 0; row < rows; ++row) {
        double mean = 0.0;
        for (std::size_t d = 0; d < width; ++d)
            mean += input.values()[row * width + d];
        mean /= static_cast<double>(width);
        double variance = 0.0;
        for (std::size_t d = 0; d < width; ++d) {
            const auto delta = input.values()[row * width + d] - mean;
            variance += delta * delta;
        }
        const auto inverse = 1.0 / std::sqrt(variance / static_cast<double>(width) + 1.0e-5);
        for (std::size_t d = 0; d < width; ++d)
            values[row * width + d] =
                (input.values()[row * width + d] - mean) * inverse * weight[d] + bias[d];
    }
    return Tensor(input.dtype(), {shape.begin(), shape.end()}, std::move(values),
                  std::string(input.device()));
}

Tensor descriptor_pair(const std::pair<Tensor, Tensor>& descriptors) {
    const auto left = descriptors.first.shape();
    const auto right = descriptors.second.shape();
    if (left.size() != 2 || !std::ranges::equal(left, right))
        throw std::invalid_argument("descriptor query pair must be aligned [B,D]");
    const auto batch = static_cast<std::size_t>(left[0]);
    const auto width = static_cast<std::size_t>(left[1]);
    std::vector<double> values(batch * 2 * width);
    for (std::size_t row = 0; row < batch; ++row) {
        std::copy_n(descriptors.first.values().begin() +
                        static_cast<std::ptrdiff_t>(row * width),
                    width, values.begin() + static_cast<std::ptrdiff_t>(row * 2 * width));
        std::copy_n(descriptors.second.values().begin() +
                        static_cast<std::ptrdiff_t>(row * width),
                    width, values.begin() +
                        static_cast<std::ptrdiff_t>((row * 2 + 1) * width));
    }
    return Tensor(descriptors.first.dtype(), {batch, 2, width}, std::move(values),
                  std::string(descriptors.first.device()));
}

Tensor world_slot_slice(const Tensor& world, const std::size_t first,
                        const std::size_t count) {
    const auto shape = world.shape();
    if (shape.size() != 3 || first + count > shape[1])
        throw std::invalid_argument("unified World slot slice is outside the workspace");
    const auto batch = static_cast<std::size_t>(shape[0]);
    const auto slots = static_cast<std::size_t>(shape[1]);
    const auto width = static_cast<std::size_t>(shape[2]);
    std::vector<double> values(batch * count * width);
    for (std::size_t row = 0; row < batch; ++row)
        std::copy_n(world.values().begin() +
                        static_cast<std::ptrdiff_t>((row * slots + first) * width),
                    count * width,
                    values.begin() + static_cast<std::ptrdiff_t>(row * count * width));
    return Tensor(world.dtype(), {batch, count, width}, std::move(values),
                  std::string(world.device()));
}

Tensor scalar_tensor(const double value, const Tensor& reference) {
    return Tensor(reference.dtype(), {}, {value}, std::string(reference.device()));
}

BooleanMask body_mask(const BooleanMask& source_mask) {
    const auto shape = source_mask.shape();
    if (shape.size() != 2 || shape[1] < 1)
        throw std::invalid_argument("text source mask must include BOS");
    const auto batch = static_cast<std::size_t>(shape[0]);
    const auto body = static_cast<std::size_t>(shape[1] - 1);
    std::vector<std::uint8_t> values(batch * body);
    for (std::size_t row = 0; row < batch; ++row)
        std::copy_n(source_mask.values().begin() +
                        static_cast<std::ptrdiff_t>(row * (body + 1) + 1),
                    body, values.begin() + static_cast<std::ptrdiff_t>(row * body));
    return BooleanMask({batch, body}, std::move(values));
}

BooleanMask cross_modal_text_mask(const BooleanMask& source_mask) {
    const auto shape = source_mask.shape();
    if (shape.size() != 2 || shape[1] < 1)
        throw std::invalid_argument("text source mask must include BOS");
    auto values = std::vector<std::uint8_t>(source_mask.values().begin(),
                                             source_mask.values().end());
    const auto batch = static_cast<std::size_t>(shape[0]);
    const auto tokens = static_cast<std::size_t>(shape[1]);
    for (std::size_t row = 0; row < batch; ++row) {
        bool any_body = false;
        for (std::size_t token = 1; token < tokens; ++token)
            any_body = any_body || values[row * tokens + token] != 0;
        values[row * tokens] = static_cast<std::uint8_t>(!any_body);
    }
    return BooleanMask({shape.begin(), shape.end()}, std::move(values));
}

WorldState replace_world_slots(const WorldState& source, Tensor slots) {
    return WorldState(
        std::move(slots), source.active_mask().clone(), source.dirty_mask().clone(),
        std::string(source.source()),
        std::vector<SurfaceResidualRef>(source.surface_refs().begin(),
                                        source.surface_refs().end()));
}

WorldConfig world_config(const MosaicUnifiedConfig& config) {
    return WorldConfig{config.omni.world_slots, config.omni.world_dim,
                       config.omni.object_slots};
}

MosaicOmniWorldPipelineConfig world_pipeline_config(
    const MosaicUnifiedConfig& config) {
    return {
        world_config(config),
        config.omni.attention_heads,
        static_cast<std::size_t>(config.world_ffn_dim),
        static_cast<std::size_t>(config.world_rounds),
        config.omni.object_slots,
        config.video_explicit_temporal_delta_scale,
        config.audio_temporal_binary_head,
        config.video_object_pair_trajectory_binding,
        config.video_descriptor_trajectory_binding,
    };
}

MosaicOmniTextPipelineConfig text_pipeline_config(
    const MosaicUnifiedConfig& config) {
    return {
        config.omni.world_dim,
        static_cast<std::size_t>(config.text_answerability_classes),
        static_cast<std::size_t>(config.text_epistemic_memory_slots),
        static_cast<std::size_t>(config.text_epistemic_supported_class),
        config.text_epistemic_output_threshold,
        config.text_answerability_mode,
        config.text_answerability_head,
    };
}

bool answerability_contextual(const std::string_view mode) {
    return mode == "contextual-cross" || mode == "consistency-cross" ||
           mode == "body-cross";
}

bool answerability_consistency(const std::string_view mode) {
    return mode == "consistency-cross" || mode == "core-lexical-consistency-cross";
}

bool answerability_body_only(const std::string_view mode) {
    return mode == "body-cross" || mode == "core-body-cross" ||
           mode == "core-compact-body-cross" ||
           mode == "core-projected-compact-body-cross" ||
           mode == "core-lexical-compact-body-cross" ||
           mode == "core-lexical-consistency-cross";
}

bool answerability_projected(const std::string_view mode) {
    return mode == "core-projected-compact-body-cross" ||
           mode == "core-lexical-compact-body-cross" ||
           mode == "core-lexical-consistency-cross";
}

bool answerability_lexical(const std::string_view mode) {
    return mode == "core-lexical-compact-body-cross" ||
           mode == "core-lexical-consistency-cross";
}

bool answerability_cross(const std::string_view mode) {
    return mode == "token-cross" || mode == "contextual-cross" ||
           mode == "consistency-cross" || answerability_body_only(mode);
}

TextAnswerabilityConfig answerability_config(const MosaicUnifiedConfig& config) {
    const auto projected = answerability_projected(config.text_answerability_mode);
    return {
        config.omni.world_dim,
        config.omni.attention_heads,
        projected ? config.text.model_dim : config.omni.world_dim,
        answerability_lexical(config.text_answerability_mode)
            ? answerability_ngram_widths.size() : 0,
        static_cast<std::size_t>(config.text_answerability_classes),
        answerability_contextual(config.text_answerability_mode),
        answerability_consistency(config.text_answerability_mode),
        answerability_body_only(config.text_answerability_mode),
    };
}

DescriptorConditionedDenseTrajectoryConfig descriptor_trajectory_config(
    const MosaicUnifiedConfig& config) {
    return {
        config.omni.world_dim,
        config.video_descriptor_pair_centered_queries,
        config.video_descriptor_persistent_identity_state,
        config.video_descriptor_object_memory,
        config.video_descriptor_object_memory_scale,
        config.video_descriptor_object_memory_query_gate,
        config.video_descriptor_object_memory_reliability_gate,
        config.video_descriptor_object_memory_contrast_visibility,
        config.video_descriptor_object_memory_contrast_readout,
        config.video_descriptor_object_memory_temporal_relative_visibility,
        config.video_descriptor_object_memory_temporal_relative_readout,
    };
}

}  // namespace

class MosaicUnifiedForConditionalGeneration::Impl final {
public:
    Impl(MosaicUnifiedConfig config_value, MosaicUnifiedWeights weights_value)
        : config(std::move(config_value)), weights(std::move(weights_value)),
          text_core(config.text, weights.text) {
        config.validate();
        initialize_modules();
    }

    [[nodiscard]] MosaicUnifiedOutput forward(const MosaicUnifiedInput& input) const;
    [[nodiscard]] Tensor encode_visual_summary(const Tensor& pixel_values) const;
    [[nodiscard]] Tensor encode_visual_regions(
        const Tensor& pixel_values, const Tensor& boxes_xyxy_normalized) const;
    [[nodiscard]] Tensor encode_visual_region_grids(
        const Tensor& pixel_values, const Tensor& boxes_xyxy_normalized,
        std::size_t grid_size) const;
    [[nodiscard]] NarrativeContinuityOutput score_narrative_continuity(
        const MosaicTokenBatch& first, const MosaicTokenBatch& second) const;
    [[nodiscard]] LongVideoWorldOutput accumulate_long_video(
        const Tensor& clip_world_states, const BooleanMask& clip_mask,
        const WorldState* initial_state) const;

    void initialize_modules();

    MosaicUnifiedConfig config;
    MosaicUnifiedWeights weights;
    MosaicTextLM text_core;
    MosaicUnifiedMaterializedWeights::RuntimeBoundaryWeights runtime_weights;
    ModalLinearWeights text_to_world_boundary;
    NarrativeDescriptorProjection descriptor_projection;
    std::shared_ptr<MosaicOmniModalFrontends> frontends;
    std::unique_ptr<ModalToWorldAdapter> to_world;
    std::unique_ptr<VisualTeacherSlotBridge> visual_teacher;
    std::unique_ptr<ExplicitObjectRelationGrounder> relation_grounder;
    std::unique_ptr<ExplicitRelationHead> relation_head;
    std::unique_ptr<MosaicOmniWorldPipeline> world_pipeline;
    std::unique_ptr<CrossModalEvidenceFusion> cross_modal;
    std::unique_ptr<TextOnlyCrossMemoryAdapter> text_cross_memory;
    std::unique_ptr<TextOnlyHiddenCrossMemoryAdapter> text_hidden_cross_memory;
    std::unique_ptr<TextEpistemicOutputAdapter> text_epistemic_output;
    std::unique_ptr<TextAnswerabilityVerifier> text_answerability;
    std::unique_ptr<MosaicOmniTextPipeline> text_pipeline;
    std::unique_ptr<NarrativeContinuityScorer> narrative;
    std::unique_ptr<LongVideoWorldAccumulator> long_video;
    std::shared_ptr<LearnedVideoObjectTracker> video_object_tracker;
    std::shared_ptr<QueryConditionedObjectTrajectoryBinding> video_trajectory;
    std::shared_ptr<QueryConditionedObjectPairTrajectoryBinding> video_pair_trajectory;
    std::shared_ptr<DescriptorConditionedDenseTrajectoryBinding> video_descriptor_trajectory;
    std::shared_ptr<VideoSpatialGeometryReasoner> video_spatial_geometry;
    std::shared_ptr<VideoEgomotionReasoner> video_egomotion;
    std::unique_ptr<MosaicOmniVideoPipeline> video_pipeline;
    std::shared_ptr<LanguageGenerationBackend> language_backend;
};

void MosaicUnifiedForConditionalGeneration::Impl::initialize_modules() {
    auto materialized = materialize_mosaic_unified(weights.parameters, config);
    runtime_weights = std::move(materialized.runtime);

    const auto modal_config = MosaicOmniModalFrontendConfig::from_unified(config);
    frontends = std::make_shared<MosaicOmniModalFrontends>(
        modal_config, std::move(materialized.frontends));

    const ModalToWorldConfig to_world_config{
        config.omni.world_dim, world_config(config), config.omni.attention_heads};
    if (!materialized.to_world)
        throw std::runtime_error("unified ModalToWorld weights are unavailable");
    to_world = std::make_unique<ModalToWorldAdapter>(
        to_world_config, std::move(*materialized.to_world));

    if (materialized.visual_teacher)
        visual_teacher = std::make_unique<VisualTeacherSlotBridge>(
            config.omni.world_dim,
            static_cast<std::size_t>(config.visual_teacher_slot_rank),
            std::move(*materialized.visual_teacher));
    if (materialized.relation_grounder)
        relation_grounder = std::make_unique<ExplicitObjectRelationGrounder>(
            config.omni.world_dim,
            static_cast<std::size_t>(config.explicit_object_relation_rank),
            std::move(*materialized.relation_grounder));
    if (materialized.relation_head)
        relation_head = std::make_unique<ExplicitRelationHead>(
            config.omni.world_dim,
            static_cast<std::size_t>(config.explicit_relation_classes),
            std::move(*materialized.relation_head));

    descriptor_projection = {
        config.omni.world_dim,
        std::vector<float>(materialized.text_pipeline.text_to_world.weight.begin(),
                           materialized.text_pipeline.text_to_world.weight.end()),
        std::vector<float>(materialized.text_pipeline.text_to_world.bias.begin(),
                           materialized.text_pipeline.text_to_world.bias.end()),
    };
    text_to_world_boundary = {
        materialized.text_pipeline.text_to_world.weight,
        materialized.text_pipeline.text_to_world.bias,
    };

    world_pipeline = std::make_unique<MosaicOmniWorldPipeline>(
        world_pipeline_config(config), std::move(materialized.world_pipeline),
        MosaicOmniWorldPipelineModules{
            to_world.get(), visual_teacher.get(), relation_grounder.get(), relation_head.get()});

    if (materialized.cross_modal)
        cross_modal = std::make_unique<CrossModalEvidenceFusion>(
            config, std::move(*materialized.cross_modal));

    if (materialized.text_cross_memory)
        text_cross_memory = std::make_unique<TextOnlyCrossMemoryAdapter>(
            config.text.model_dim, config.text.attention_heads,
            std::move(*materialized.text_cross_memory));
    if (materialized.text_hidden_cross_memory)
        text_hidden_cross_memory =
            std::make_unique<TextOnlyHiddenCrossMemoryAdapter>(
                config.text.model_dim, config.text.attention_heads,
                std::move(*materialized.text_hidden_cross_memory));
    if (materialized.text_epistemic_output)
        text_epistemic_output = std::make_unique<TextEpistemicOutputAdapter>(
            config.text.model_dim,
            static_cast<std::size_t>(config.text_epistemic_output_rank),
            std::move(*materialized.text_epistemic_output));
    if (materialized.text_answerability)
        text_answerability = std::make_unique<TextAnswerabilityVerifier>(
            answerability_config(config), std::move(*materialized.text_answerability));

    text_pipeline = std::make_unique<MosaicOmniTextPipeline>(
        text_core, text_pipeline_config(config),
        std::move(materialized.text_pipeline),
        MosaicOmniTextPipelineAdapters{
            text_answerability.get(), text_cross_memory.get(),
            text_hidden_cross_memory.get(), text_epistemic_output.get()});

    if (materialized.narrative)
        narrative = std::make_unique<NarrativeContinuityScorer>(
            text_core, std::move(*materialized.narrative));
    if (materialized.long_video)
        long_video = std::make_unique<LongVideoWorldAccumulator>(
            config.omni, config.long_video_transition_features,
            std::move(*materialized.long_video));

    if (materialized.video.object_tracker)
        video_object_tracker = std::make_shared<LearnedVideoObjectTracker>(
            config.omni.object_slots, config.omni.world_dim,
            config.video_object_spatial_coordinates,
            std::move(*materialized.video.object_tracker));
    if (materialized.video.trajectory)
        video_trajectory =
            std::make_shared<QueryConditionedObjectTrajectoryBinding>(
                config.omni.world_dim, std::move(*materialized.video.trajectory));
    if (materialized.video.pair_trajectory)
        video_pair_trajectory =
            std::make_shared<QueryConditionedObjectPairTrajectoryBinding>(
                config.omni.world_dim,
                std::move(*materialized.video.pair_trajectory));
    if (materialized.video.descriptor_trajectory)
        video_descriptor_trajectory =
            std::make_shared<DescriptorConditionedDenseTrajectoryBinding>(
                descriptor_trajectory_config(config),
                std::move(*materialized.video.descriptor_trajectory));
    if (materialized.video.spatial_geometry)
        video_spatial_geometry = std::make_shared<VideoSpatialGeometryReasoner>(
            static_cast<std::size_t>(config.video_camera_pose_dim),
            std::move(*materialized.video.spatial_geometry));
    if (materialized.video.egomotion)
        video_egomotion = std::make_shared<VideoEgomotionReasoner>(
            config.omni.world_dim, std::move(*materialized.video.egomotion));

    video_pipeline = std::make_unique<MosaicOmniVideoPipeline>(
        VideoPipelineConfig::from_unified(config),
        std::move(materialized.video.pipeline),
        VideoPipelineModules{
            frontends, video_object_tracker, video_trajectory,
            video_pair_trajectory, video_descriptor_trajectory,
            video_spatial_geometry, video_egomotion});
}

MosaicUnifiedOutput MosaicUnifiedForConditionalGeneration::Impl::forward(
    const MosaicUnifiedInput& input) const {
    validate_bos_batch(input.input_ids, "input_ids");
    const auto batch = input.input_ids.size();
    const auto& world_ids = input.world_input_ids ? *input.world_input_ids : input.input_ids;
    validate_bos_batch(world_ids, "world_input_ids", batch);
    if (input.question_input_ids)
        validate_bos_batch(*input.question_input_ids, "question_input_ids", batch);
    const bool text_only = !input.pixel_values && !input.audio_values && !input.video_values;

    auto text_source = text_pipeline->prepare_text_source({world_ids, text_only});
    const auto text_body_mask = body_mask(text_source.source_mask);
    std::vector<Tensor> source_tokens{text_source.source_tokens.clone()};
    std::vector<BooleanMask> source_masks{text_source.source_mask.clone()};
    std::vector<std::string> modalities{text_source.modality};

    std::optional<ImageFrontendOutput> image;
    if (input.pixel_values) {
        image = frontends->forward_image(*input.pixel_values, batch);
        source_tokens.push_back(image->tokens.clone());
        source_masks.push_back(image->mask.clone());
        modalities.emplace_back("image");
    }

    std::optional<AudioFrontendOutput> audio;
    if (input.audio_values) {
        audio = frontends->forward_audio(*input.audio_values, batch);
        source_tokens.push_back(audio->tokens.clone());
        source_masks.push_back(audio->mask.clone());
        modalities.emplace_back("audio");
    }

    std::optional<VideoPipelineInput> video_input;
    std::optional<VideoPrepared> video;
    if (input.video_values) {
        video_input.emplace(VideoPipelineInput{
            input.video_values->clone(),
            input.camera_pose_values ? std::optional<Tensor>(input.camera_pose_values->clone())
                                     : std::nullopt,
            input.action_values ? std::optional<Tensor>(input.action_values->clone())
                                : std::nullopt});
        video = video_pipeline->prepare(*video_input);
        source_tokens.push_back(video->source_tokens.clone());
        source_masks.push_back(video->source_mask.clone());
        modalities.emplace_back("video");
    } else if (input.camera_pose_values || input.action_values) {
        throw std::invalid_argument("camera pose and action values require video input");
    }

    MosaicOmniWorldPipelineInputs world_request;
    world_request.source_tokens = std::move(source_tokens);
    world_request.source_masks = std::move(source_masks);
    world_request.modalities = modalities;
    if (video) {
        world_request.camera_pose_states = video->camera_pose_states;
        world_request.action_summary = video->action_summary;
        world_request.video_object_slots = video->object_slots;
        world_request.video_temporal_summary = video->temporal_summary;
        world_request.video_temporal_delta = video->temporal_delta;
    }
    world_request.text_world = text_source.text_world.clone();
    world_request.subject_descriptor_mask = input.subject_descriptor_mask;
    world_request.object_descriptor_mask = input.object_descriptor_mask;
    world_request.subject_descriptor_input_ids = input.subject_descriptor_input_ids;
    world_request.object_descriptor_input_ids = input.object_descriptor_input_ids;
    world_request.descriptor_text_core = &text_core;
    world_request.descriptor_projection = &descriptor_projection;
    if (image) {
        world_request.image_patch_tokens = image->patch_tokens;
        world_request.image_patch_grid = std::pair{image->patch_height, image->patch_width};
    }
    if (audio) {
        world_request.audio_world_summary = audio->world_summary;
        world_request.audio_temporal_features = audio->temporal_features;
        world_request.audio_content_features = audio->content_features;
    }
    auto world_result = world_pipeline->forward(world_request);

    std::optional<CrossModalEvidenceOutput> cross;
    if (cross_modal && (audio || video)) {
        Tensor evidence_text_tokens = text_source.text_world.clone();
        BooleanMask evidence_text_mask = cross_modal_text_mask(text_source.source_mask);
        if (config.cross_modal_text_contextual_pooling) {
            const auto contextual = text_core.forward(
                world_ids, nullptr, std::optional<std::size_t>{1});
            evidence_text_tokens = linear_boundary(
                contextual.context_states, text_to_world_boundary,
                config.omni.world_dim);
            evidence_text_mask = text_body_mask.clone();
        }
        std::optional<Tensor> audio_summary;
        std::optional<Tensor> audio_tokens;
        if (audio) {
            audio_summary = audio->content_summary ? audio->content_summary :
                                                    std::optional<Tensor>(audio->summary);
            audio_tokens = audio->content_features ? audio->content_features :
                           audio->temporal_features ? audio->temporal_features :
                           std::optional<Tensor>(audio->evidence_tokens);
        }
        std::optional<Tensor> video_summary;
        std::optional<Tensor> video_tokens;
        if (video) {
            video_summary = video->temporal_summary;
            video_tokens = video->object_world_slots ? video->object_world_slots :
                                                       std::optional<Tensor>(video->evidence_tokens);
        }
        cross = cross_modal->forward(CrossModalEvidenceInput{
            text_source.retrieval_summary.clone(), std::move(evidence_text_tokens),
            std::move(evidence_text_mask), std::move(audio_summary),
            std::move(audio_tokens), std::move(video_summary),
            std::move(video_tokens)});
    }

    auto normalized_world = layer_norm_boundary(
        world_result.world_state.semantic_slots(), runtime_weights.world_norm_weight,
        runtime_weights.world_norm_bias);
    auto world_state = replace_world_slots(
        world_result.world_state, normalized_world.clone());
    world_state.validate(world_config(config));

    std::optional<VideoPipelineOutput> video_output;
    if (video && video_input) {
        std::optional<Tensor> text_query;
        if (config.video_query_conditioned_head)
            text_query = text_source.retrieval_summary;
        std::optional<Tensor> descriptors;
        if (world_result.descriptor_queries)
            descriptors = descriptor_pair(*world_result.descriptor_queries);
        video_output = video_pipeline->decide(
            *video_input, *video,
            VideoDecisionInput{normalized_world.clone(), std::move(text_query),
                               std::move(descriptors)});
    }

    // The original final heads use the normalized World event slots and the
    // selected text retrieval source. Re-running the pure frontend preserves
    // those values without adding a second storage or policy path.
    Tensor text_audio_source = text_source.retrieval_summary.clone();
    if (config.audio_text_retrieval_text_source == "world_global")
        text_audio_source = world_slot_slice(
            normalized_world, config.omni.world_slots - 1, 1);
    if (text_audio_source.shape().size() == 3) {
        text_audio_source = Tensor(
            text_audio_source.dtype(), {text_audio_source.shape()[0],
                                         text_audio_source.shape()[2]},
            {text_audio_source.values().begin(), text_audio_source.values().end()},
            std::string(text_audio_source.device()));
    }
    std::optional<Tensor> text_audio_retrieval_embedding;
    if (config.audio_text_retrieval_head)
        text_audio_retrieval_embedding =
            frontends->project_text_audio_retrieval(text_audio_source);
    if (input.audio_values) {
        auto event_slots = world_slot_slice(normalized_world, 26, 2);
        audio = frontends->forward_audio(*input.audio_values, batch,
                                         &text_audio_source, &event_slots);
    }

    auto text_result = text_pipeline->forward(MosaicOmniTextPipelineInputs{
        input.input_ids, world_ids, input.question_input_ids, input.targets,
        input.answerability_labels, input.text_rounds, text_only,
        normalized_world.clone(), text_source.text_states.clone(),
        text_body_mask.clone()});

    MosaicOmniOutputInputs output{
        std::move(text_result.text), std::move(world_state), modalities};
    if (video_output) {
        output.video_order_logits = std::move(video_output->order_logits);
        output.video_object_evidence_weights =
            std::move(video_output->object_evidence_weights);
        output.video_object_attention = std::move(video_output->object_attention);
        output.video_object_trajectory_weights =
            std::move(video_output->object_trajectory_weights);
        output.video_object_pair_trajectory_weights =
            std::move(video_output->object_pair_trajectory_weights);
        output.video_descriptor_trajectory_attention =
            std::move(video_output->descriptor_trajectory_attention);
        output.video_descriptor_visibility_logits =
            std::move(video_output->descriptor_visibility_logits);
        output.video_descriptor_memory_margin =
            std::move(video_output->descriptor_memory_margin);
        output.video_descriptor_memory_reliability_logits =
            std::move(video_output->descriptor_memory_reliability_logits);
        output.video_camera_robustness_gate =
            std::move(video_output->camera_robustness_gate);
        output.video_spatial_relation_logits =
            std::move(video_output->spatial_relation_logits);
        output.video_egomotion_logits = std::move(video_output->egomotion_logits);
        output.video_egomotion_validity_logits =
            std::move(video_output->egomotion_validity_logits);
        output.video_egomotion_motion_evidence =
            std::move(video_output->egomotion_motion_evidence);
        output.video_egomotion_sufficient_mask =
            std::move(video_output->egomotion_sufficient_mask);
        output.video_teacher_embedding =
            std::move(video_output->teacher_embedding);
    }
    if (audio) {
        output.audio_temporal_logits = audio->temporal_logits;
        output.audio_embedding = audio->content_summary ? audio->content_summary :
                                 std::optional<Tensor>(audio->summary);
        output.audio_teacher_embedding = audio->teacher_embedding;
        output.audio_teacher_temporal_states = audio->teacher_temporal_states;
        output.audio_world_teacher_embedding = audio->world_teacher_embedding;
        output.audio_ctc_logits = audio->ctc_logits;
        output.audio_grapheme_ctc_logits = audio->grapheme_ctc_logits;
        output.audio_text_retrieval_embedding =
            audio->audio_text_retrieval_embedding;
    }
    output.text_audio_retrieval_embedding =
        std::move(text_audio_retrieval_embedding);
    if (video) {
        output.video_embedding = video->world_summary;
        output.visual_embedding = video->world_summary;
    } else if (image) {
        output.visual_embedding = image->summary;
    }
    output.text_retrieval_embedding = text_source.retrieval_summary;
    if (cross) {
        output.cross_modal_evidence_logits = std::move(cross->logits);
        output.cross_modal_evidence_delta = std::move(cross->world_delta);
    }
    if (runtime_weights.visual_text_retrieval_projection && output.visual_embedding)
        output.visual_text_retrieval_embedding = linear_boundary(
            *output.visual_embedding,
            *runtime_weights.visual_text_retrieval_projection,
            static_cast<std::size_t>(config.visual_text_retrieval_dim));
    if (runtime_weights.text_visual_retrieval_projection)
        output.text_visual_retrieval_embedding = linear_boundary(
            text_source.retrieval_summary,
            *runtime_weights.text_visual_retrieval_projection,
            static_cast<std::size_t>(config.visual_text_retrieval_dim));
    output.answerability_logits = std::move(text_result.answerability_logits);
    if (text_result.answerability_loss)
        output.answerability_loss = scalar_tensor(
            *text_result.answerability_loss, output.text.logits);
    output.explicit_relation_logits =
        std::move(world_result.explicit_relation_logits);
    output.explicit_object_attention =
        std::move(world_result.explicit_object_attention);
    return assemble_mosaic_omni_output(std::move(output));
}

Tensor MosaicUnifiedForConditionalGeneration::Impl::encode_visual_summary(
    const Tensor& pixel_values) const {
    return frontends->encode_visual_summary(pixel_values);
}

Tensor MosaicUnifiedForConditionalGeneration::Impl::encode_visual_regions(
    const Tensor& pixel_values, const Tensor& boxes_xyxy_normalized) const {
    return frontends->encode_visual_regions(pixel_values, boxes_xyxy_normalized);
}

Tensor MosaicUnifiedForConditionalGeneration::Impl::encode_visual_region_grids(
    const Tensor& pixel_values, const Tensor& boxes_xyxy_normalized,
    const std::size_t grid_size) const {
    return frontends->encode_visual_region_grids(
        pixel_values, boxes_xyxy_normalized, grid_size);
}

NarrativeContinuityOutput
MosaicUnifiedForConditionalGeneration::Impl::score_narrative_continuity(
    const MosaicTokenBatch& first, const MosaicTokenBatch& second) const {
    if (!narrative) throw std::runtime_error("narrative evidence head is disabled");
    return narrative->score_narrative_continuity(first, second);
}

LongVideoWorldOutput MosaicUnifiedForConditionalGeneration::Impl::accumulate_long_video(
    const Tensor& clip_world_states, const BooleanMask& clip_mask,
    const WorldState* initial_state) const {
    if (!long_video) throw std::runtime_error("long-video World accumulator is disabled");
    return long_video->forward(clip_world_states, clip_mask, initial_state);
}

MosaicUnifiedForConditionalGeneration::MosaicUnifiedForConditionalGeneration(
    MosaicUnifiedConfig config, MosaicUnifiedWeights weights)
    : impl_(std::make_unique<Impl>(std::move(config), std::move(weights))) {}

MosaicUnifiedForConditionalGeneration::~MosaicUnifiedForConditionalGeneration() = default;
MosaicUnifiedForConditionalGeneration::MosaicUnifiedForConditionalGeneration(
    MosaicUnifiedForConditionalGeneration&&) noexcept = default;
MosaicUnifiedForConditionalGeneration&
MosaicUnifiedForConditionalGeneration::operator=(
    MosaicUnifiedForConditionalGeneration&&) noexcept = default;

const MosaicUnifiedConfig& MosaicUnifiedForConditionalGeneration::config() const noexcept {
    return impl_->config;
}

MosaicUnifiedOutput MosaicUnifiedForConditionalGeneration::forward(
    const MosaicUnifiedInput& input) const {
    return impl_->forward(input);
}

MosaicTokenBatch MosaicUnifiedForConditionalGeneration::generate(
    const MosaicUnifiedGenerateInput& input) const {
    if (input.maximum_new_bytes < 0)
        throw std::invalid_argument("maximum_new_bytes must not be negative");
    const auto maximum_new_bytes =
        static_cast<std::size_t>(input.maximum_new_bytes);
    if (impl_->language_backend) {
        if (input.pixel_values || input.audio_values || input.video_values ||
            input.use_answerability_gate ||
            (input.world_input_ids && *input.world_input_ids != input.input_ids))
            throw std::invalid_argument(
                "configured language backend requires sealed text evidence; native modal/gate inputs unsupported");
        if (!input.language_snapshot_id)
            throw std::invalid_argument("configured language backend requires current snapshot callback");
        return generate_language_proposal(*impl_->language_backend, input.input_ids,
            maximum_new_bytes, input.language_requests, input.language_snapshot_id);
    }
    if (!input.language_requests.empty() || input.language_snapshot_id)
        throw std::invalid_argument(
            "main-bound provider requests supplied without configured backend");

    validate_bos_batch(input.input_ids, "input_ids");
    const auto batch = input.input_ids.size();
    const auto& world_ids = input.world_input_ids ? *input.world_input_ids : input.input_ids;
    validate_bos_batch(world_ids, "world_input_ids", batch);
    std::vector<std::uint8_t> forced_rows(batch, 0);
    if (input.use_answerability_gate) {
        MosaicUnifiedInput probe;
        probe.input_ids = input.input_ids; probe.world_input_ids = world_ids;
        probe.pixel_values = input.pixel_values; probe.audio_values = input.audio_values;
        probe.video_values = input.video_values; probe.question_input_ids = input.input_ids;
        probe.text_rounds = input.text_rounds;
        const auto output = impl_->forward(probe);
        if (!output.answerability_logits)
            throw std::runtime_error("answerability logits are unavailable");
        const auto shape = output.answerability_logits->shape();
        if (shape.size() != 2 || shape[0] != batch ||
            shape[1] != static_cast<std::uint64_t>(impl_->config.text_answerability_classes))
            throw std::runtime_error("answerability logits shape changed");
        for (std::size_t b = 0; b < batch; ++b) {
            const auto classes = static_cast<std::size_t>(shape[1]);
            double maximum = -std::numeric_limits<double>::infinity();
            for (std::size_t c = 0; c < classes; ++c)
                maximum = std::max(maximum, output.answerability_logits->values()[b * classes + c]);
            std::vector<double> probability(classes); double denominator = 0.0;
            for (std::size_t c = 0; c < classes; ++c) {
                probability[c] = std::exp(output.answerability_logits->values()[b * classes + c] - maximum);
                denominator += probability[c];
            }
            for (auto& value : probability) value /= denominator;
            forced_rows[b] = classes == 2
                ? probability[1] < impl_->config.text_answerability_threshold
                : 1.0 - probability[static_cast<std::size_t>(impl_->config.text_epistemic_supported_class)] >=
                    impl_->config.text_epistemic_output_threshold;
        }
    }

    auto generated = input.input_ids;
    std::vector<std::uint8_t> finished(batch, 0);
    for (std::size_t step = 0; step < maximum_new_bytes; ++step) {
        auto candidate = generated;
        for (auto& row : candidate) row.push_back(mosaic_pad_id);
        MosaicUnifiedInput request;
        request.input_ids = std::move(candidate); request.world_input_ids = world_ids;
        request.pixel_values = input.pixel_values; request.audio_values = input.audio_values;
        request.video_values = input.video_values; request.question_input_ids = input.input_ids;
        request.text_rounds = input.text_rounds;
        const auto output = impl_->forward(request);
        const auto shape = output.logits().shape();
        if (shape.size() != 4 || shape[0] != batch || shape[3] != mosaic_vocab_size)
            throw std::runtime_error("unified text logits shape changed");
        const auto positions = static_cast<std::size_t>(shape[1] * shape[2]);
        const auto position = generated.front().size() - 1;
        if (position >= positions) throw std::runtime_error("generated position exceeds logits");
        for (std::size_t b = 0; b < batch; ++b) {
            std::int64_t next = mosaic_eos_id;
            if (!finished[b]) {
                if (forced_rows[b]) {
                    next = step < impl_->config.text_answerability_fallback_bytes.size()
                        ? impl_->config.text_answerability_fallback_bytes[step] : mosaic_eos_id;
                } else {
                    double best = -std::numeric_limits<double>::infinity();
                    const auto base = (b * positions + position) * mosaic_vocab_size;
                    for (std::size_t token = 0; token < static_cast<std::size_t>(mosaic_vocab_size); ++token) {
                        if (token == static_cast<std::size_t>(mosaic_pad_id) ||
                            token == static_cast<std::size_t>(mosaic_bos_id)) continue;
                        if (output.logits().values()[base + token] > best) {
                            best = output.logits().values()[base + token];
                            next = static_cast<std::int64_t>(token);
                        }
                    }
                }
            }
            generated[b].push_back(next); finished[b] = finished[b] || next == mosaic_eos_id;
        }
        if (std::ranges::all_of(finished, [](const auto value) { return value != 0; })) break;
    }
    return generated;
}

Tensor MosaicUnifiedForConditionalGeneration::encode_visual_summary(
    const Tensor& pixel_values) const { return impl_->encode_visual_summary(pixel_values); }
Tensor MosaicUnifiedForConditionalGeneration::encode_visual_regions(
    const Tensor& pixel_values, const Tensor& boxes_xyxy_normalized) const {
    return impl_->encode_visual_regions(pixel_values, boxes_xyxy_normalized);
}
Tensor MosaicUnifiedForConditionalGeneration::encode_visual_region_grids(
    const Tensor& pixel_values, const Tensor& boxes_xyxy_normalized,
    const std::size_t grid_size) const {
    return impl_->encode_visual_region_grids(
        pixel_values, boxes_xyxy_normalized, grid_size);
}
NarrativeContinuityOutput MosaicUnifiedForConditionalGeneration::score_narrative_continuity(
    const MosaicTokenBatch& first, const MosaicTokenBatch& second) const {
    return impl_->score_narrative_continuity(first, second);
}
LongVideoWorldOutput MosaicUnifiedForConditionalGeneration::accumulate_long_video(
    const Tensor& clip_world_states, const BooleanMask& clip_mask,
    const WorldState* initial_state) const {
    return impl_->accumulate_long_video(clip_world_states, clip_mask, initial_state);
}
void MosaicUnifiedForConditionalGeneration::configure_language_backend(
    const std::optional<std::filesystem::path>& config_path,
    const std::optional<std::string>& profile) {
    if (!config_path) {
        if (profile) throw std::invalid_argument("native backend does not take a provider profile");
        impl_->language_backend.reset();
        return;
    }
    if (!profile) throw std::invalid_argument("enabled language backend profile required");
    auto facade = load_facade(*config_path);
    configure_language_registry(std::move(facade.providers), *profile);
}

void MosaicUnifiedForConditionalGeneration::configure_language_registry(
    std::shared_ptr<const ProviderRegistry> registry, std::string profile) {
    if (!registry) throw std::invalid_argument("provider registry is required");
    bool enabled = false;
    for (const auto& row_value : registry->describe()) {
        const auto& row = row_value.as_object();
        if (row.at("name").as_string() == profile) {
            const auto& state = row.at("enabled").storage();
            enabled = std::holds_alternative<bool>(state) && std::get<bool>(state);
            break;
        }
    }
    if (!enabled) throw std::invalid_argument("enabled language backend profile required");
    impl_->language_backend = std::make_shared<ProfiledProposalEngine>(
        std::move(registry), std::move(profile));
}

}  // namespace swegca::world
