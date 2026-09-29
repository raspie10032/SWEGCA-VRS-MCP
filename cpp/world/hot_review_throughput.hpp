#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view hot_review_throughput_source_sha256 =
    "ad22aee7f0a981eef98f0e2d5a8471afad5bfd72789b71b4d2fd339fe1c418dc";

struct HotEvidenceMatrix final {
    std::vector<float> values;
    std::size_t rows{};
    std::size_t columns{};
};

struct VectorizedEvidenceScores final {
    std::vector<float> support;
    std::vector<float> refutation;
    std::vector<float> residual;
    std::uint64_t comparison_count{};
    bool persistent_state_mutated{};
};

[[nodiscard]] VectorizedEvidenceScores vectorized_evidence_scores(
    const HotEvidenceMatrix& evidence_keys,
    std::span<const float> support_query,
    std::span<const float> refutation_query = {});

}  // namespace swegca::world
