#include "world/modal_to_world.hpp"
#include "modal_to_world_python214_fixture.hpp"
#include "swegca_architecture/sha256.hpp"

#include <algorithm>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using swegca::checkpoint::MaterializedTensor;
using swegca::checkpoint::RestrictedCheckpoint;
using swegca::checkpoint::checkpoint_profiles;
using CheckpointDType = swegca::checkpoint::TensorDType;
using swegca::world::BooleanMask;
using swegca::world::ModalToWorldAdapter;
using swegca::world::ModalToWorldConfig;
using swegca::world::ModalToWorldWeights;
using swegca::world::Tensor;
using WorldDType = swegca::world::TensorDType;
using swegca::world::WorldConfig;

std::string digest_hex(const swegca::architecture::DigestBytes& digest) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result(digest.size() * 2U, '0');
    for (std::size_t index = 0; index != digest.size(); ++index) {
        const auto value = std::to_integer<unsigned>(digest[index]);
        result[index * 2U] = digits[value >> 4U];
        result[index * 2U + 1U] = digits[value & 15U];
    }
    return result;
}

MaterializedTensor materialized(std::vector<std::uint64_t> shape,
                                const std::vector<float>& values) {
    std::vector<std::byte> bytes(values.size() * sizeof(float));
    for (std::size_t index = 0; index != values.size(); ++index) {
        const auto bits = std::bit_cast<std::uint32_t>(values[index]);
        for (unsigned byte = 0; byte != sizeof(float); ++byte) {
            bytes[index * sizeof(float) + byte] =
                static_cast<std::byte>((bits >> (byte * 8U)) & 0xffU);
        }
    }
    return MaterializedTensor(CheckpointDType::float32, std::move(shape),
                              std::move(bytes));
}

ModalToWorldConfig config() {
    return ModalToWorldConfig{4, WorldConfig{32, 4, 8}, 2};
}

ModalToWorldWeights weights() {
    constexpr std::size_t slots = 32;
    constexpr std::size_t width = 4;
    std::vector<float> queries(slots * width);
    for (std::size_t slot = 0; slot != slots; ++slot) {
        queries[slot * width] = static_cast<float>(slot + 1U) * 0.01F;
        queries[slot * width + 1U] = -0.02F;
        queries[slot * width + 2U] = 0.03F;
        queries[slot * width + 3U] = -0.04F;
    }
    const std::vector<float> ones(width, 1.0F);
    const std::vector<float> zero_width(width, 0.0F);
    std::vector<float> identity(width * width, 0.0F);
    for (std::size_t index = 0; index != width; ++index) {
        identity[index * width + index] = 1.0F;
    }
    std::vector<float> packed(3U * width * width, 0.0F);
    for (std::size_t block = 0; block != 3U; ++block) {
        for (std::size_t index = 0; index != width; ++index) {
            packed[(block * width + index) * width + index] = 1.0F;
        }
    }
    return ModalToWorldWeights(
        materialized({slots, width}, queries), materialized({width}, ones),
        materialized({width}, zero_width),
        materialized({width, width}, identity),
        materialized({width}, zero_width),
        materialized({3U * width, width}, packed),
        materialized({3U * width}, std::vector<float>(3U * width, 0.0F)),
        materialized({width, width}, identity),
        materialized({width}, zero_width), materialized({width}, ones),
        materialized({width}, zero_width), config());
}

Tensor input(const bool perturb_masked = false) {
    std::vector<double> values{
        1.0, 2.0, 4.0, 8.0,
        2.0, 3.0, 5.0, 7.0,
        3.0, 1.0, 6.0, 2.0,
        4.0, 2.0, 1.0, 3.0,
        2.0, 6.0, 3.0, 1.0,
        8.0, 4.0, 2.0, 1.0,
    };
    if (perturb_masked) {
        values[4] = -9000.0;
        values[5] = 7000.0;
        values[6] = 5000.0;
        values[7] = -3000.0;
        values[20] = 12345.0;
        values[21] = -23456.0;
        values[22] = 34567.0;
        values[23] = -45678.0;
    }
    return Tensor(WorldDType::float32, {2, 3, 4}, std::move(values));
}

template <typename Function>
void rejects(Function&& function, const std::string& message) {
    try {
        function();
        assert(false);
    } catch (const std::invalid_argument& error) {
        assert(std::string(error.what()).find(message) != std::string::npos);
    }
}

void test_omitted_mask_equals_all_true_and_output_contract() {
    const ModalToWorldAdapter adapter(config(), weights());
    const auto source = input();
    const auto before = source.clone();
    const BooleanMask all_true({2, 3}, std::vector<std::uint8_t>(6, 1U));
    const auto omitted = adapter.forward(source, "unified:text+image");
    const auto explicit_mask =
        adapter.forward(source, &all_true, "unified:text+image");

    assert(source.exact_equal(before));
    assert(omitted.exact_equal(explicit_mask));
    assert(std::vector<std::uint64_t>(omitted.semantic_slots().shape().begin(),
                                      omitted.semantic_slots().shape().end()) ==
           std::vector<std::uint64_t>({2, 32, 4}));
    assert(std::vector<std::uint64_t>(omitted.active_mask().shape().begin(),
                                      omitted.active_mask().shape().end()) ==
           std::vector<std::uint64_t>({2, 32}));
    assert(omitted.source() == "unified:text+image");
    for (const auto value : omitted.active_mask().values()) assert(value == 1U);
    for (const auto value : omitted.dirty_mask().values()) assert(value == 0U);
    for (const auto value : omitted.semantic_slots().values()) assert(std::isfinite(value));
}

void test_masked_token_perturbation_is_invariant() {
    const ModalToWorldAdapter adapter(config(), weights());
    const BooleanMask mask({2, 3}, {1, 0, 1, 1, 1, 0});
    const auto baseline = adapter.forward(input(), &mask, "masked");
    const auto perturbed = adapter.forward(input(true), &mask, "masked");
    assert(baseline.exact_equal(perturbed));
}

void test_shape_rejections() {
    auto mismatched = config();
    mismatched.attention_heads = 1;
    rejects([&] {
        (void)ModalToWorldAdapter(mismatched, weights());
    }, "weights and adapter config differ");
    const ModalToWorldAdapter adapter(config(), weights());
    rejects([&] {
        const Tensor bad(WorldDType::float32, {3, 4},
                         std::vector<double>(12, 0.0));
        (void)adapter.forward(bad, "bad");
    }, "shape [batch, tokens, dim]");
    rejects([&] {
        const Tensor bad(WorldDType::float32, {1, 2, 5},
                         std::vector<double>(10, 0.0));
        (void)adapter.forward(bad, "bad");
    }, "final dimension");
    rejects([&] {
        const BooleanMask bad_mask({2, 2}, std::vector<std::uint8_t>(4, 1U));
        (void)adapter.forward(input(), &bad_mask, "bad");
    }, "source_mask");
    bool empty_rejected = false;
    try {
        const Tensor empty(WorldDType::float32, {1, 0, 4}, {});
        (void)adapter.forward(empty, "empty");
    } catch (const std::runtime_error&) {
        empty_rejected = true;
    }
    assert(empty_rejected);
}

void test_all_masked_row_matches_sdpa_zero_attention() {
    const ModalToWorldAdapter adapter(config(), weights());
    const BooleanMask none({2, 3}, std::vector<std::uint8_t>(6, 0U));
    const auto result = adapter.forward(input(), &none, "all-masked");
    for (const auto value : result.semantic_slots().values()) assert(std::isfinite(value));
}

void test_pinned_checkpoint_python_activation_fixture() {
    const auto& profile = checkpoint_profiles().front();
    assert(profile.checkpoint_sha256 ==
           swegca::test_fixture::modal_checkpoint_sha256);
    assert(profile.model_config_canonical_sha256 ==
           swegca::test_fixture::modal_canonical_config_sha256);
    auto checkpoint = RestrictedCheckpoint::open(profile.original_path);
    const ModalToWorldConfig pinned{256, WorldConfig{32, 256, 8}, 8};
    const ModalToWorldConfig wrong_heads{256, WorldConfig{32, 256, 8}, 4};
    rejects([&] { (void)ModalToWorldWeights::load(checkpoint, wrong_heads); },
            "audited checkpoint profile");
    const ModalToWorldAdapter adapter(
        pinned, ModalToWorldWeights::load(checkpoint, pinned));
    std::vector<double> values(3U * 256U);
    std::vector<std::byte> input_bytes(values.size() * sizeof(float));
    for (std::size_t index = 0; index != values.size(); ++index) {
        values[index] = static_cast<double>(static_cast<int>(index % 37U) - 18) / 19.0;
        const auto bits = std::bit_cast<std::uint32_t>(static_cast<float>(values[index]));
        for (unsigned byte = 0; byte != sizeof(float); ++byte) {
            input_bytes[index * sizeof(float) + byte] =
                static_cast<std::byte>((bits >> (byte * 8U)) & 0xffU);
        }
    }
    assert(digest_hex(swegca::architecture::Sha256::of(input_bytes)) ==
           swegca::test_fixture::modal_input_sha256);
    const Tensor source(WorldDType::float32, {1, 3, 256}, std::move(values));
    const BooleanMask mask({1, 3}, {1, 0, 1});
    const auto result = adapter.forward(source, &mask, "golden");
    const auto output = result.semantic_slots().values();

    // PyTorch 2.14.0+cpu, one thread, deterministic algorithms, audited
    // checkpoint 041dfd..., identical input formula and mask.  The scalar C++
    // reduction is compared numerically; backend bit identity is not claimed.
    assert(output.size() == swegca::test_fixture::modal_output_bits.size());
    std::vector<std::byte> expected_bytes(output.size() * sizeof(float));
    double maximum_absolute_error = 0.0;
    for (std::size_t index = 0; index != output.size(); ++index) {
        const auto bits = swegca::test_fixture::modal_output_bits[index];
        for (unsigned byte = 0; byte != sizeof(float); ++byte) {
            expected_bytes[index * sizeof(float) + byte] =
                static_cast<std::byte>((bits >> (byte * 8U)) & 0xffU);
        }
        const auto expected = static_cast<double>(std::bit_cast<float>(bits));
        assert(std::isfinite(output[index]) && std::isfinite(expected));
        maximum_absolute_error = std::max(
            maximum_absolute_error, std::abs(output[index] - expected));
    }
    assert(digest_hex(swegca::architecture::Sha256::of(expected_bytes)) ==
           swegca::test_fixture::modal_output_sha256);
    // This scalar implementation measured 1.66893e-6 against the pinned
    // PyTorch CPU fixture; keep a small allowance for libm/compiler variation.
    assert(maximum_absolute_error <= 3.0e-6);
    std::cout << "pinned Python/C++ maximum absolute error: "
              << maximum_absolute_error << '\n';

    const BooleanMask all_masked({1, 3}, {0, 0, 0});
    const auto masked = adapter.forward(source, &all_masked, "all-masked");
    const auto masked_output = masked.semantic_slots().values();
    assert(masked_output.size() ==
           swegca::test_fixture::modal_all_masked_output_bits.size());
    std::vector<std::byte> masked_expected_bytes(
        masked_output.size() * sizeof(float));
    double masked_maximum_absolute_error = 0.0;
    for (std::size_t index = 0; index != masked_output.size(); ++index) {
        const auto bits =
            swegca::test_fixture::modal_all_masked_output_bits[index];
        for (unsigned byte = 0; byte != sizeof(float); ++byte) {
            masked_expected_bytes[index * sizeof(float) + byte] =
                static_cast<std::byte>((bits >> (byte * 8U)) & 0xffU);
        }
        const auto expected = static_cast<double>(std::bit_cast<float>(bits));
        assert(std::isfinite(masked_output[index]) && std::isfinite(expected));
        masked_maximum_absolute_error = std::max(
            masked_maximum_absolute_error,
            std::abs(masked_output[index] - expected));
    }
    assert(digest_hex(swegca::architecture::Sha256::of(masked_expected_bytes)) ==
           swegca::test_fixture::modal_all_masked_output_sha256);
    assert(masked_maximum_absolute_error <= 3.0e-6);
    std::cout << "all-masked Python/C++ maximum absolute error: "
              << masked_maximum_absolute_error << '\n';
}

}  // namespace

int main() {
    test_omitted_mask_equals_all_true_and_output_contract();
    test_masked_token_perturbation_is_invariant();
    test_shape_rejections();
    test_all_masked_row_matches_sdpa_zero_attention();
    test_pinned_checkpoint_python_activation_fixture();
    std::cout << "modal_to_world_tests: ok\n";
}
