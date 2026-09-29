#include "world/hot_review_throughput.hpp"

#include <cmath>
#include <limits>
#include <stdexcept>

namespace swegca::world {
namespace {

bool finite(std::span<const float> values) {
    for (const auto value : values) if (!std::isfinite(value)) return false;
    return true;
}

float norm(std::span<const float> values) {
    float total = 0.0F;
    for (const auto value : values) total += value * value;
    return std::sqrt(total);
}

}  // namespace

VectorizedEvidenceScores vectorized_evidence_scores(
    const HotEvidenceMatrix& evidence_keys,
    const std::span<const float> support_query,
    std::span<const float> refutation_query) {
    if (refutation_query.empty()) refutation_query = support_query;
    if (evidence_keys.columns != support_query.size() ||
        evidence_keys.columns != refutation_query.size() ||
        evidence_keys.values.size() != evidence_keys.rows * evidence_keys.columns)
        throw std::invalid_argument("hot evidence and query dimensions changed");
    if (!finite(evidence_keys.values) || !finite(support_query) || !finite(refutation_query))
        throw std::invalid_argument("hot evidence review requires finite values");

    const auto support_norm = norm(support_query);
    const auto refutation_norm = norm(refutation_query);
    VectorizedEvidenceScores result;
    result.support.reserve(evidence_keys.rows);
    result.refutation.reserve(evidence_keys.rows);
    result.residual.reserve(evidence_keys.rows);
    for (std::size_t row = 0; row < evidence_keys.rows; ++row) {
        const auto values = std::span<const float>(evidence_keys.values).subspan(
            row * evidence_keys.columns, evidence_keys.columns);
        const auto row_norm = norm(values);
        float support = 0.0F;
        float refutation = 0.0F;
        if (row_norm != 0.0F && support_norm != 0.0F)
            for (std::size_t column = 0; column < values.size(); ++column)
                support += (values[column] / row_norm) *
                           (support_query[column] / support_norm);
        if (row_norm != 0.0F && refutation_norm != 0.0F)
            for (std::size_t column = 0; column < values.size(); ++column)
                refutation += (values[column] / row_norm) *
                              (refutation_query[column] / refutation_norm);
        result.support.push_back(support);
        result.refutation.push_back(refutation);
        result.residual.push_back(support - refutation);
    }
    if (evidence_keys.rows > std::numeric_limits<std::uint64_t>::max() / 2)
        throw std::overflow_error("hot evidence comparison count overflow");
    result.comparison_count = static_cast<std::uint64_t>(evidence_keys.rows) * 2;
    return result;
}

}  // namespace swegca::world
