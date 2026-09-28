#include "world/recurrent_cognition.hpp"

#include "recurrent_cognition_python214_fixture.hpp"
#include "swegca_architecture/sha256.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <vector>

using namespace swegca::world;
namespace fixture = swegca::world::test_fixture;

namespace {

template <typename T, std::size_t Size>
std::vector<T> vector_of(const std::array<T, Size>& values) {
    return {values.begin(), values.end()};
}

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

RecurrentCognitionWeights weights() {
    RecurrentCognitionWeights result;
    result.attention_norm_weight = vector_of(fixture::weight_cell_attention_norm_weight);
    result.attention_norm_bias = vector_of(fixture::weight_cell_attention_norm_bias);
    result.attention_in_projection_weight = vector_of(fixture::weight_cell_attention_in_proj_weight);
    result.attention_in_projection_bias = vector_of(fixture::weight_cell_attention_in_proj_bias);
    result.attention_out_projection_weight = vector_of(fixture::weight_cell_attention_out_proj_weight);
    result.attention_out_projection_bias = vector_of(fixture::weight_cell_attention_out_proj_bias);
    result.mlp_norm_weight = vector_of(fixture::weight_cell_mlp_norm_weight);
    result.mlp_norm_bias = vector_of(fixture::weight_cell_mlp_norm_bias);
    result.mlp_in_weight = vector_of(fixture::weight_cell_mlp_in_weight);
    result.mlp_out_weight = vector_of(fixture::weight_cell_mlp_out_weight);
    result.update_gate_weight = vector_of(fixture::weight_cell_update_gate_weight);
    result.update_gate_bias = vector_of(fixture::weight_cell_update_gate_bias);
    result.role_embeddings = vector_of(fixture::weight_role_embeddings);
    result.final_norm_weight = vector_of(fixture::weight_final_norm_weight);
    result.final_norm_bias = vector_of(fixture::weight_final_norm_bias);
    result.halt_head_weight = vector_of(fixture::weight_halt_head_weight);
    result.halt_head_bias = vector_of(fixture::weight_halt_head_bias);
    result.halt_evidence_scale = fixture::weight_halt_evidence_scale.front();
    return result;
}

RecurrentCognitionConfig config() {
    return {{2, 1, 1, 4}, 2, 3, 2, 3, 0.5F, 1.0e-4F, 0.7F};
}

Tensor tensor(std::vector<std::uint64_t> shape, const std::span<const float> values) {
    std::vector<double> converted;
    converted.reserve(values.size());
    for (const auto value : values) converted.push_back(value);
    return {TensorDType::float32, std::move(shape), std::move(converted), "cpu"};
}

CognitiveState state() {
    return {
        tensor({2, 2, 4}, fixture::input_semantic),
        tensor({2, 1, 4}, fixture::input_executive),
        tensor({2, 1, 4}, fixture::input_scratch),
        StructuredWorldGraph({WorldEntity("fixture-node", "fixture")}),
        {"old-a", "old-b"},
        {{"goal", "fixture"}}, {{"value", 2}}, {{"self", "fixture"}},
        "fixture-owner"};
}

RecurrentEvidence evidence() {
    return {
        tensor({2, 2, 4}, fixture::input_evidence),
        BooleanMask({2, 2}, vector_of(fixture::input_mask)),
        vector_of(fixture::input_coverage),
        vector_of(fixture::input_confidence),
        vector_of(fixture::input_keyweights)};
}

std::vector<float> flattened(const CognitiveState& value) {
    std::vector<float> result;
    for (const Tensor* partition : {&value.semantic_slots(), &value.executive_slots(),
                                    &value.scratch_slots()}) {
        for (const auto item : partition->values()) result.push_back(static_cast<float>(item));
    }
    return result;
}

template <std::size_t Size>
void close(const std::span<const float> observed, const std::array<float, Size>& expected,
           const float tolerance, float& maximum_error) {
    assert(observed.size() == expected.size());
    for (std::size_t index = 0; index != expected.size(); ++index) {
        assert(std::isfinite(observed[index]));
        const auto error = std::abs(observed[index] - expected[index]);
        maximum_error = std::max(maximum_error, error);
        assert(error <= tolerance);
    }
}

template <std::size_t StateSize, std::size_t BatchSize>
void check_output(const RecurrentCognitionOutput& output,
                  const std::array<float, StateSize>& expected_state,
                  const std::array<std::uint64_t, BatchSize>& expected_cycles,
                  const std::array<std::array<float, BatchSize>, 3>& expected_logits,
                  const std::array<std::array<float, BatchSize>, 3>& expected_probabilities,
                  float& maximum_error) {
    const auto values = flattened(output.state);
    close(values, expected_state, 4.0e-6F, maximum_error);
    assert(std::equal(output.trace.cycles_used.begin(), output.trace.cycles_used.end(),
                      expected_cycles.begin(), expected_cycles.end()));
    assert(output.trace.halt_logits.size() == 3);
    assert(output.trace.halt_probabilities.size() == 3);
    for (std::size_t cycle = 0; cycle != 3; ++cycle) {
        close(output.trace.halt_logits[cycle], expected_logits[cycle], 1.0e-6F, maximum_error);
        close(output.trace.halt_probabilities[cycle], expected_probabilities[cycle],
              1.0e-6F, maximum_error);
    }
}

void fixture_parity() {
    assert(std::string_view(fixture::source_sha256) ==
           "5ea32f8d6b97ae8db013e09b1cb4d42f49e9c1cd1794d8d8f5bad40ea519ef6e");
    assert(hex(swegca::architecture::Sha256::of(std::as_bytes(
        std::span(fixture::expected_eval_state)))) == fixture::expected_eval_state_sha256);
    assert(hex(swegca::architecture::Sha256::of(std::as_bytes(
        std::span(fixture::expected_zero_state)))) == fixture::expected_zero_state_sha256);

    const RecurrentCognitionCore core(config(), weights());
    const auto original = state();
    const auto original_copy = original.clone();
    const auto transient = evidence();
    const auto eval = core.run(original, &transient);
    const auto training = core.run(original, &transient,
        RecurrentExecutionMode::training_selection);
    const auto zero = core.run(original);
    assert(original.exact_equal(original_copy));
    assert(eval.state.structured_world_graph() == original.structured_world_graph());
    assert(eval.state.goal_state() == original.goal_state());
    assert(eval.state.value_state() == original.value_state());
    assert(eval.state.self_state() == original.self_state());
    assert(eval.state.owner_id() == original.owner_id());
    assert(eval.state.persistent_state_count() == 1);

    float maximum_error = 0.0F;
    check_output(eval, fixture::expected_eval_state, fixture::expected_eval_cycles,
        {fixture::expected_eval_logits_0, fixture::expected_eval_logits_1,
         fixture::expected_eval_logits_2},
        {fixture::expected_eval_probabilities_0, fixture::expected_eval_probabilities_1,
         fixture::expected_eval_probabilities_2}, maximum_error);
    check_output(training, fixture::expected_training_state, fixture::expected_training_cycles,
        {fixture::expected_training_logits_0, fixture::expected_training_logits_1,
         fixture::expected_training_logits_2},
        {fixture::expected_training_probabilities_0,
         fixture::expected_training_probabilities_1,
         fixture::expected_training_probabilities_2}, maximum_error);
    // The third training cycle advances the already halted first row; evaluation
    // freezes it. Both return the first selected halt state, but their traces differ.
    assert(eval.trace.halt_logits[2][0] != training.trace.halt_logits[2][0]);
    check_output(zero, fixture::expected_zero_state, fixture::expected_zero_cycles,
        {fixture::expected_zero_logits_0, fixture::expected_zero_logits_1,
         fixture::expected_zero_logits_2},
        {fixture::expected_zero_probabilities_0, fixture::expected_zero_probabilities_1,
         fixture::expected_zero_probabilities_2}, maximum_error);
    std::cout << "recurrent Python 2.14 fixture max absolute error: "
              << maximum_error << '\n';
}

void evidence_reference_contract() {
    const RecurrentCognitionCore core(config(), weights());
    const auto original = state();
    const auto transient = evidence();
    const std::array<std::string, 3> references{"old-b", "new-c", "new-d"};
    const auto accepted = integrate_authorized_evidence(
        core, original, transient, references, true, 3);
    assert((std::vector<std::string>(accepted.state.evidence_refs().begin(),
                                    accepted.state.evidence_refs().end()) ==
            std::vector<std::string>{"old-b", "new-c", "new-d"}));
    const auto denied = integrate_authorized_evidence(
        core, original, transient, references, false, 3);
    assert(std::equal(denied.state.evidence_refs().begin(), denied.state.evidence_refs().end(),
                      original.evidence_refs().begin(), original.evidence_refs().end()));
    assert(original.evidence_refs().size() == 2);
}

template <typename Function>
void rejects(Function&& function) {
    bool rejected = false;
    try { function(); } catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
}

void validation_contract() {
    auto invalid = config();
    invalid.attention_heads = 3;
    rejects([&] { invalid.validate(); });
    invalid = config();
    invalid.minimum_cycles = 0;
    rejects([&] { invalid.validate(); });
    invalid = config();
    invalid.maximum_update = std::numeric_limits<float>::infinity();
    rejects([&] { invalid.validate(); });

    auto bad_weights = weights();
    bad_weights.mlp_in_weight.pop_back();
    rejects([&] { RecurrentCognitionCore ignored(config(), std::move(bad_weights)); });

    const RecurrentCognitionCore core(config(), weights());
    auto bad_evidence = evidence();
    (*bad_evidence.attention_key_weights)[0] = 0.0F;
    rejects([&] { static_cast<void>(core.run(state(), &bad_evidence)); });
    bad_evidence = evidence();
    (*bad_evidence.confidence)[1] = std::numeric_limits<float>::quiet_NaN();
    rejects([&] { static_cast<void>(core.run(state(), &bad_evidence)); });
    rejects([&] { static_cast<void>(core.run(
        state(), nullptr, static_cast<RecurrentExecutionMode>(255))); });

    const auto complete_evidence = evidence();
    RecurrentEvidence missing_wrapper_fields{
        complete_evidence.tokens->clone(), complete_evidence.mask,
        std::nullopt, std::nullopt, std::nullopt};
    const std::array<std::string, 1> valid_ref{"experience://valid"};
    rejects([&] { static_cast<void>(integrate_authorized_evidence(
        core, state(), missing_wrapper_fields, valid_ref, true, 2)); });

    RecurrentEvidence rank_zero{
        Tensor(TensorDType::float32, {}, {0.0}, "cpu"),
        BooleanMask({1, 1}, {1}), std::vector<float>{1.0F},
        std::vector<float>{1.0F}, std::nullopt};
    rejects([&] { static_cast<void>(integrate_authorized_evidence(
        core, state(), rank_zero, valid_ref, false, 2)); });

    const std::array<std::string, 1> blank_ref{"  "};
    rejects([&] { static_cast<void>(integrate_authorized_evidence(
        core, state(), evidence(), blank_ref, false, 2)); });
    const std::array<std::string, 1> unicode_blank{"\xe3\x80\x80"};
    rejects([&] { static_cast<void>(integrate_authorized_evidence(
        core, state(), evidence(), unicode_blank, false, 2)); });
    const std::array<std::string, 1> invalid_utf8{std::string("\xc0\x80", 2)};
    rejects([&] { static_cast<void>(integrate_authorized_evidence(
        core, state(), evidence(), invalid_utf8, false, 2)); });
}

void coverage_only_and_halt_contract() {
    auto threshold_config = config();
    threshold_config.minimum_cycles = 1;
    auto threshold_weights = weights();
    std::fill(threshold_weights.halt_head_weight.begin(),
              threshold_weights.halt_head_weight.end(), 0.0F);
    threshold_weights.halt_head_bias.front() = 0.0F;
    const RecurrentCognitionCore core(threshold_config, std::move(threshold_weights));

    // A supplied mask is ignored when tokens are absent, matching the source.
    RecurrentEvidence coverage_only{
        std::nullopt, BooleanMask({1, 1}, {1}),
        std::vector<float>{0.5F, 0.5F}, std::nullopt, std::nullopt};
    const auto output = core.run(state(), &coverage_only);
    assert(output.trace.halt_probabilities.size() == 1);
    assert(output.trace.cycles_used == std::vector<std::uint64_t>({1, 1}));
    assert(output.trace.halt_probabilities[0] == std::vector<float>({0.5F, 0.5F}));
}

void masked_and_nonfinite_attention_contract() {
    const RecurrentCognitionCore core(config(), weights());
    const auto original = evidence();
    const auto baseline = core.run(state(), &original);

    auto masked_finite = evidence();
    auto masked_values = std::vector<double>(masked_finite.tokens->values().begin(),
                                             masked_finite.tokens->values().end());
    masked_values[4] = 12'345.0;
    masked_finite.tokens = Tensor(TensorDType::float32, {2, 2, 4},
                                  std::move(masked_values), "cpu");
    const auto masked = core.run(state(), &masked_finite);
    assert(masked.state.exact_equal(baseline.state));
    assert(masked.trace.cycles_used == baseline.trace.cycles_used);
    assert(masked.trace.halt_logits == baseline.trace.halt_logits);
    assert(masked.trace.halt_probabilities == baseline.trace.halt_probabilities);

    // PyTorch's value matmul preserves IEEE 0*NaN propagation even when that
    // key is masked. Do not clean this dirty input into a finite experience.
    auto masked_nonfinite = evidence();
    auto masked_nan_values = std::vector<double>(masked_nonfinite.tokens->values().begin(),
                                                 masked_nonfinite.tokens->values().end());
    masked_nan_values[4] = std::numeric_limits<double>::quiet_NaN();
    masked_nonfinite.tokens = Tensor(TensorDType::float32, {2, 2, 4},
                                     std::move(masked_nan_values), "cpu");
    const auto masked_nan = core.run(state(), &masked_nonfinite);
    const auto masked_nan_state = flattened(masked_nan.state);
    assert(std::any_of(masked_nan_state.begin(), masked_nan_state.end(),
                       [](const float value) { return !std::isfinite(value); }));

    auto active_nonfinite = evidence();
    auto active_values = std::vector<double>(active_nonfinite.tokens->values().begin(),
                                             active_nonfinite.tokens->values().end());
    active_values[0] = std::numeric_limits<double>::quiet_NaN();
    active_nonfinite.tokens = Tensor(TensorDType::float32, {2, 2, 4},
                                     std::move(active_values), "cpu");
    const auto active = core.run(state(), &active_nonfinite);
    const auto values = flattened(active.state);
    assert(std::any_of(values.begin(), values.end(),
                       [](const float value) { return !std::isfinite(value); }));
}

void evidence_weight_and_confidence_contract() {
    const RecurrentCognitionCore core(config(), weights());
    auto unweighted = evidence();
    unweighted.attention_key_weights.reset();
    auto unit_weighted = unweighted;
    unit_weighted.attention_key_weights = std::vector<float>(4, 1.0F);
    const auto default_output = core.run(state(), &unweighted);
    const auto unit_output = core.run(state(), &unit_weighted);
    const auto default_values = flattened(default_output.state);
    const auto unit_values = flattened(unit_output.state);
    assert(default_values.size() == unit_values.size());
    for (std::size_t index = 0; index != default_values.size(); ++index) {
        assert(std::abs(default_values[index] - unit_values[index]) <= 1.0e-6F);
    }

    const auto full = evidence();
    std::vector<double> single_values;
    for (std::size_t row = 0; row != 2; ++row) {
        const auto begin = full.tokens->values().begin() +
                           static_cast<std::ptrdiff_t>(row * 2U * 4U);
        single_values.insert(single_values.end(), begin, begin + 4);
    }
    std::vector<double> repeated_values;
    for (std::size_t row = 0; row != 2; ++row) {
        for (std::size_t repeat = 0; repeat != 4; ++repeat) {
            repeated_values.insert(repeated_values.end(),
                single_values.begin() + static_cast<std::ptrdiff_t>(row * 4U),
                single_values.begin() + static_cast<std::ptrdiff_t>((row + 1U) * 4U));
        }
    }
    RecurrentEvidence repeated{
        Tensor(TensorDType::float32, {2, 4, 4}, std::move(repeated_values), "cpu"),
        BooleanMask({2, 4}, std::vector<std::uint8_t>(8, 1)),
        std::vector<float>{1.0F, 1.0F}, std::vector<float>{1.0F, 1.0F}, std::nullopt};
    RecurrentEvidence multiplicity{
        Tensor(TensorDType::float32, {2, 1, 4}, std::move(single_values), "cpu"),
        BooleanMask({2, 1}, {1, 1}), std::vector<float>{1.0F, 1.0F},
        std::vector<float>{1.0F, 1.0F}, std::vector<float>{4.0F, 4.0F}};
    const auto repeated_output = flattened(core.run(state(), &repeated).state);
    const auto weighted_output = flattened(core.run(state(), &multiplicity).state);
    assert(repeated_output.size() == weighted_output.size());
    for (std::size_t index = 0; index != repeated_output.size(); ++index) {
        assert(std::abs(repeated_output[index] - weighted_output[index]) <= 1.0e-6F);
    }

    auto confidence_config = config();
    confidence_config.minimum_cycles = 1;
    confidence_config.maximum_cycles = 4;
    auto confidence_weights = weights();
    std::fill(confidence_weights.halt_head_weight.begin(),
              confidence_weights.halt_head_weight.end(), 0.0F);
    confidence_weights.halt_head_bias.front() = 0.0F;
    const RecurrentCognitionCore confidence_core(
        confidence_config, std::move(confidence_weights));
    RecurrentEvidence implicit{
        std::nullopt, std::nullopt, std::vector<float>{1.0F, 1.0F},
        std::nullopt, std::nullopt};
    auto explicit_ones = implicit;
    explicit_ones.confidence = std::vector<float>{1.0F, 1.0F};
    const auto implicit_output = confidence_core.run(state(), &implicit);
    const auto explicit_output = confidence_core.run(state(), &explicit_ones);
    assert(implicit_output.state.exact_equal(explicit_output.state));
    assert(implicit_output.trace.cycles_used == explicit_output.trace.cycles_used);

    auto split_confidence = implicit;
    split_confidence.confidence = std::vector<float>{1.0F, 0.1F};
    const auto split_output = confidence_core.run(state(), &split_confidence);
    assert(split_output.trace.cycles_used == std::vector<std::uint64_t>({1, 4}));
}

}  // namespace

int main() {
    fixture_parity();
    evidence_reference_contract();
    validation_contract();
    coverage_only_and_halt_contract();
    masked_and_nonfinite_attention_contract();
    evidence_weight_and_confidence_contract();
    std::cout << "recurrent cognition tests passed\n";
}
