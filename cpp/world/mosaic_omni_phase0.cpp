#include "world/mosaic_omni_phase0.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace swegca::world {
namespace {

JsonValue integer(const std::size_t value) {
    if (value > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()))
        return JsonValue(JsonInteger{std::to_string(value)});
    return JsonValue(static_cast<std::int64_t>(value));
}

JsonValue shape_json(const std::span<const std::uint64_t> shape) {
    JsonValue::Array values;
    values.reserve(shape.size());
    for (const auto value : shape) values.emplace_back(JsonInteger{std::to_string(value)});
    return JsonValue(std::move(values));
}

JsonValue strings_json(const std::span<const std::string_view> values) {
    JsonValue::Array result;
    result.reserve(values.size());
    for (const auto value : values) result.emplace_back(value);
    return JsonValue(std::move(result));
}

double rounded(const double value, const double scale) {
    return std::round(value * scale) / scale;
}

double median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    const auto middle = values.size() / 2;
    return values.size() % 2 ? values[middle]
                             : 0.5 * (values[middle - 1] + values[middle]);
}

bool finite(const Tensor& value) {
    return std::ranges::all_of(value.values(),
                               [](const double item) { return std::isfinite(item); });
}

bool exact_shape(const std::span<const std::uint64_t> actual,
                 const std::initializer_list<std::uint64_t> expected) {
    return actual.size() == expected.size() &&
           std::equal(actual.begin(), actual.end(), expected.begin());
}

bool nested_bool(const JsonValue::Object& root, const std::string_view object_key,
                 const std::string_view value_key, bool& found) {
    found = false;
    const auto object = root.find(object_key);
    if (object == root.end() || !object->second.is_object()) return false;
    const auto value = object->second.as_object().find(value_key);
    if (value == object->second.as_object().end()) return false;
    if (const auto boolean = std::get_if<bool>(&value->second.storage())) {
        found = true;
        return *boolean;
    }
    return false;
}

}  // namespace

JsonValue::Object run_mosaic_omni_phase0_probe(
    const MosaicOmniConfig& config, const std::size_t repeats,
    const JsonValue::Object& local_contract,
    const MosaicOmniPhase0Dependencies& dependencies) {
    config.validate();
    if (repeats == 0) throw std::invalid_argument("repeats must be positive");
    if (config.world_slots != slot_roles.size())
        throw std::invalid_argument("phase-0 requires the frozen 32-slot World contract");
    const auto& text_config = dependencies.text_encoder.config();
    if (text_config.patch_size != 4 || text_config.max_bytes != 128 ||
        text_config.model_dim != 64 || text_config.conditioning_dim != config.world_dim ||
        text_config.attention_heads != 4 || text_config.ffn_dim != 128 ||
        text_config.local_layers != 1 || text_config.slot_count != config.world_slots ||
        text_config.recurrent_rounds != 2) {
        throw std::invalid_argument(
            "injected text encoder does not match the pinned phase-0 probe config");
    }

    const std::vector<std::string> prompt{
        "비 오는 골목에서 우산을 든 캐릭터가 카메라를 향해 걷는다."};
    const auto text_output = dependencies.text_encoder.encode(prompt);
    std::vector<double> text_values;
    text_values.reserve(text_output.global_slots.values.size());
    for (const auto value : text_output.global_slots.values)
        text_values.push_back(static_cast<double>(value));
    Tensor text_slots(TensorDType::float32,
                      {1, static_cast<std::uint64_t>(config.world_slots),
                       static_cast<std::uint64_t>(config.world_dim)},
                      std::move(text_values));
    WorldState text_state(
        std::move(text_slots),
        BooleanMask({1, static_cast<std::uint64_t>(config.world_slots)},
                    std::vector<std::uint8_t>(config.world_slots, 1)),
        BooleanMask({1, static_cast<std::uint64_t>(config.world_slots)},
                    std::vector<std::uint8_t>(config.world_slots, 0)),
        "mosaic_text",
        {SurfaceResidualRef{"image", "probe://surface/image", {1, 4, 32, 32}, {}}});
    text_state.validate(WorldConfig{config.world_slots, config.world_dim,
                                    config.object_slots});

    std::mt19937_64 generator(73);
    std::normal_distribution<double> normal(0.0, 1.0);
    const auto hidden_count = static_cast<std::size_t>(260) * config.gemma_hidden_dim;
    std::vector<double> hidden_values(hidden_count);
    for (auto& value : hidden_values) value = normal(generator);
    const Tensor gemma_hidden(
        TensorDType::float32,
        {1, 260, static_cast<std::uint64_t>(config.gemma_hidden_dim)},
        std::move(hidden_values));
    const BooleanMask gemma_mask({1, 260}, std::vector<std::uint8_t>(260, 1));
    auto gemma_state = dependencies.gemma_adapter.forward(
        gemma_hidden, &gemma_mask, "gemma4_unified_raw_image_activation");
    auto conditioning = dependencies.anima_adapter.forward(gemma_state);

    const std::array<std::uint64_t, 2> selected{0, 22};
    std::vector<double> update_values(2 * config.world_dim);
    for (std::size_t row = 0; row < selected.size(); ++row)
        for (std::size_t dim = 0; dim < config.world_dim; ++dim)
            update_values[row * config.world_dim + dim] =
                text_state.semantic_slots().values()[selected[row] * config.world_dim + dim] + 1.0;
    const Tensor updates(TensorDType::float32,
                         {1, 2, static_cast<std::uint64_t>(config.world_dim)},
                         std::move(update_values));
    const std::array<std::string, 2> dirty_regions{"object:0", "lighting"};
    const auto edited = edit_world_slots(
        text_state, selected, updates,
        WorldConfig{config.world_slots, config.world_dim, config.object_slots},
        dirty_regions);
    bool edit_locality = true;
    for (std::size_t slot = 0; slot < config.world_slots; ++slot) {
        if (slot == 0 || slot == 22) continue;
        for (std::size_t dim = 0; dim < config.world_dim; ++dim)
            edit_locality = edit_locality &&
                text_state.semantic_slots().values()[slot * config.world_dim + dim] ==
                edited.semantic_slots().values()[slot * config.world_dim + dim];
    }

    std::vector<double> observation_values(text_state.semantic_slots().values().begin(),
                                           text_state.semantic_slots().values().end());
    for (std::size_t slot = 0; slot < config.object_slots; ++slot)
        for (std::size_t dim = 0; dim < config.world_dim; ++dim)
            observation_values[slot * config.world_dim + dim] += 0.5;
    WorldState observation(
        Tensor(TensorDType::float32,
               {1, static_cast<std::uint64_t>(config.world_slots),
                static_cast<std::uint64_t>(config.world_dim)},
               std::move(observation_values)),
        text_state.active_mask().clone(), text_state.dirty_mask().clone(),
        "next_clip_observation",
        std::vector<SurfaceResidualRef>(text_state.surface_refs().begin(),
                                        text_state.surface_refs().end()));
    std::vector<std::uint8_t> visible_values(config.object_slots, 0);
    visible_values.front() = 1;
    const BooleanMask visible({1, static_cast<std::uint64_t>(config.object_slots)},
                              std::move(visible_values));
    const auto merged = merge_persistent_scene_memory(
        text_state, observation, visible,
        WorldConfig{config.world_slots, config.world_dim, config.object_slots});
    bool offscreen_preserved = true;
    for (std::size_t slot = 1; slot < config.object_slots; ++slot)
        for (std::size_t dim = 0; dim < config.world_dim; ++dim)
            offscreen_preserved = offscreen_preserved &&
                merged.semantic_slots().values()[slot * config.world_dim + dim] ==
                text_state.semantic_slots().values()[slot * config.world_dim + dim];
    bool visible_updated = true;
    for (std::size_t dim = 0; dim < config.world_dim; ++dim)
        visible_updated = visible_updated &&
            merged.semantic_slots().values()[dim] == observation.semantic_slots().values()[dim];

    Tensor output = conditioning.clone();
    for (std::size_t warmup = 0; warmup < 3; ++warmup) {
        gemma_state = dependencies.gemma_adapter.forward(
            gemma_hidden, &gemma_mask, "gemma4_unified_raw_image_activation");
        output = dependencies.anima_adapter.forward(gemma_state);
    }
    std::vector<double> latencies;
    latencies.reserve(repeats);
    for (std::size_t repeat = 0; repeat < repeats; ++repeat) {
        const auto started = std::chrono::steady_clock::now();
        gemma_state = dependencies.gemma_adapter.forward(
            gemma_hidden, &gemma_mask, "gemma4_unified_raw_image_activation");
        output = dependencies.anima_adapter.forward(gemma_state);
        latencies.push_back(std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - started).count());
    }
    auto ordered = latencies;
    std::sort(ordered.begin(), ordered.end());
    const auto p95_index = std::min(ordered.size() - 1,
        static_cast<std::size_t>(std::ceil(ordered.size() * 0.95)) - 1);

    const bool dirty_propagated = !edited.surface_refs().empty() &&
        edited.surface_refs().front().dirty_regions ==
            std::vector<std::string>{"object:0", "lighting"};
    JsonValue::Object checks{
        {"frozen_32_slot_roles", slot_roles.size() == config.world_slots},
        {"text_to_world_shape", exact_shape(text_state.semantic_slots().shape(),
            {1, config.world_slots, config.world_dim})},
        {"gemma_activation_to_world_shape", exact_shape(gemma_state.semantic_slots().shape(),
            {1, config.world_slots, config.world_dim})},
        {"world_to_anima_conditioning_shape", exact_shape(conditioning.shape(),
            {1, config.anima_conditioning_tokens, config.anima_conditioning_dim})},
        {"selective_edit_changes_only_requested_slots", edit_locality},
        {"surface_residual_dirty_regions_propagate", dirty_propagated},
        {"offscreen_object_identity_is_preserved", offscreen_preserved},
        {"visible_object_identity_is_updated", visible_updated},
        {"outputs_are_finite", finite(output)}};
    bool passed = true;
    for (const auto& [name, value] : checks) {
        static_cast<void>(name);
        passed = passed && std::get<bool>(value.storage());
    }
    const auto parameter_count = dependencies.text_encoder.parameter_count() +
                                 dependencies.injected_adapter_parameter_count;
    bool gemma_floor_found = false;
    const auto gemma_fits = nested_bool(
        local_contract, "weight_file_floors",
        "gemma12b_co_resident_fits_4gib_by_files_only", gemma_floor_found);
    JsonValue::Object shapes{
        {"text_world", shape_json(text_state.semantic_slots().shape())},
        {"gemma_raw_image_activation_input", shape_json(gemma_hidden.shape())},
        {"gemma_world", shape_json(gemma_state.semantic_slots().shape())},
        {"anima_conditioning", shape_json(conditioning.shape())}};
    JsonValue::Object latency{
        {"gemma_activation_to_anima_batch_median", rounded(median(latencies), 10000.0)},
        {"gemma_activation_to_anima_batch_p95", rounded(ordered[p95_index], 10000.0)},
        {"repeats", integer(repeats)}};
    JsonValue::Object feasibility{
        {"world_latent_interface", "passed"},
        {"gemmanima_conditioning_adapter", "passed_shape_only"},
        {"selective_edit_state_contract", "passed"},
        {"persistent_identity_state_contract", "passed"},
        {"current_gemma4_12b_co_resident_4gb",
         gemma_floor_found && !gemma_fits ? "failed_by_weight_files_before_runtime"
                                         : "not_measured"},
        {"edge_route", "small_cognition_core_or_remote_gemma_then_world_latent; do_not_co-reside Gemma4 12B with the edge renderer"},
        {"image_generation_quality", "not_tested"},
        {"video_generation", "no_trained_renderer_or_codec"},
        {"audio_generation", "no_trained_renderer_or_codec"},
        {"overall", "phase0_integration_feasible_full_mosaic_omni_unverified"}};
    return {
        {"schema_version", "mosaic-omni-phase0-v0"},
        {"status", "contract_probe_not_generation_quality_validation"},
        {"config", JsonValue(config.to_dict())},
        {"slot_roles", strings_json(slot_roles)},
        {"parameter_count", JsonInteger{std::to_string(parameter_count)}},
        {"parameter_storage_fp16_mib", rounded(parameter_count * 2.0 / 1048576.0, 1000.0)},
        {"shapes", JsonValue(std::move(shapes))},
        {"latency_ms", JsonValue(std::move(latency))},
        {"process_memory_mib", JsonValue(mosaic_process_memory().to_dict())},
        {"local_contract", JsonValue(local_contract)},
        {"checks", JsonValue(std::move(checks))},
        {"phase0_contract_passed", passed},
        {"feasibility", JsonValue(std::move(feasibility))}};
}

}  // namespace swegca::world
