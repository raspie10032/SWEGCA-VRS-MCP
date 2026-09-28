#include "checkpoint/prototype_checkpoint.hpp"
#include "swegca_architecture/sha256.hpp"
#include "world/prototype_recurrent_cognition.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <string>
#include <vector>

using namespace swegca::world;

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

std::vector<float> pattern(const std::size_t count, const int modulus,
                           const int offset, const float denominator) {
    std::vector<float> result(count);
    for (std::size_t index = 0; index != count; ++index) {
        result[index] = static_cast<float>(static_cast<int>(index % modulus) - offset) /
                        denominator;
    }
    return result;
}

Tensor tensor(std::vector<std::uint64_t> shape, const std::vector<float>& values) {
    std::vector<double> converted;
    converted.reserve(values.size());
    for (const auto value : values) converted.push_back(value);
    return {TensorDType::float32, std::move(shape), std::move(converted), "cpu"};
}

std::vector<float> flattened(const CognitiveState& state) {
    std::vector<float> result;
    result.reserve(98'304);
    for (const Tensor* partition : {&state.semantic_slots(), &state.executive_slots(),
                                    &state.scratch_slots()}) {
        for (const auto value : partition->values()) result.push_back(static_cast<float>(value));
    }
    return result;
}

std::vector<float> expected_values(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    assert(input);
    std::vector<char> bytes((std::istreambuf_iterator<char>(input)), {});
    assert(bytes.size() == 98'304U * sizeof(float));
    assert(hex(swegca::architecture::Sha256::of(std::as_bytes(std::span(bytes)))) ==
           "1413f54338f1c57498de4d9f1f76194033665676aa72994ab5db17941c83859b");
    std::vector<float> values(98'304);
    std::memcpy(values.data(), bytes.data(), bytes.size());
    return values;
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
    static_assert(std::endian::native == std::endian::little,
                  "Prototype0 f32le fixture requires little endian");
    assert(file_sha256(
        "tests/fixtures/prototype_recurrent_seed631_python214_output.json") ==
        "c58567530762cec7d1b979f4e04d69d7c1224607e50c53bb2e823105038ee987");
    const auto profiles = swegca::checkpoint::prototype_checkpoint_profiles();
    auto checkpoint = swegca::checkpoint::PrototypeCheckpoint::open(
        profiles.front().original_path);
    auto bundle = load_prototype_recurrent_bundle(checkpoint);
    RecurrentCognitionCore core(bundle.config, std::move(bundle.weights));

    CognitiveState state(
        tensor({1, 32, 2048}, pattern(32U * 2048U, 257, 128, 257.0F)),
        tensor({1, 8, 2048}, pattern(8U * 2048U, 251, 125, 263.0F)),
        tensor({1, 8, 2048}, pattern(8U * 2048U, 241, 120, 239.0F)),
        StructuredWorldGraph({WorldEntity("fixture", "prototype")}),
        {"prototype://seed631"}, {}, {}, {}, "fixture");
    RecurrentEvidence evidence{
        tensor({1, 3, 2048}, pattern(3U * 2048U, 131, 65, 193.0F)),
        BooleanMask({1, 3}, {1, 0, 1}), std::vector<float>{0.72F},
        std::vector<float>{0.8F}, std::vector<float>{1.25F, 2.0F, 0.75F}};

    const auto output = core.run(state, &evidence);
    const auto observed = flattened(output.state);
    const auto expected = expected_values(
        "tests/fixtures/prototype_recurrent_seed631_python214_output.f32le");
    assert(observed.size() == expected.size());
    double squared_error = 0.0;
    float maximum_error = 0.0F;
    for (std::size_t index = 0; index != observed.size(); ++index) {
        assert(std::isfinite(observed[index]));
        const auto error = std::abs(observed[index] - expected[index]);
        maximum_error = std::max(maximum_error, error);
        squared_error += static_cast<double>(error) * error;
    }
    const auto rms_error = std::sqrt(squared_error / observed.size());
    assert(maximum_error <= 2.0e-5F);
    assert(rms_error <= 2.0e-6);
    assert(output.trace.cycles_used == std::vector<std::uint64_t>{4});
    const std::array<float, 4> expected_logits{
        -0.0405702889F, -0.0502416193F, -0.0647032857F, -0.0794090033F};
    const std::array<float, 4> expected_probabilities{
        0.489858836F, 0.487442255F, 0.483829796F, 0.48015815F};
    assert(output.trace.halt_logits.size() == expected_logits.size());
    for (std::size_t cycle = 0; cycle != expected_logits.size(); ++cycle) {
        assert(std::abs(output.trace.halt_logits[cycle].front() - expected_logits[cycle]) <=
               2.0e-5F);
        assert(std::abs(output.trace.halt_probabilities[cycle].front() -
                        expected_probabilities[cycle]) <= 5.0e-6F);
    }
    std::cout << "Prototype0 seed631 CPU single-input activation fixture parity max_error="
              << maximum_error
              << " rms_error=" << rms_error << '\n';
}
