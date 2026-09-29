#pragma once

#include "world/semantic_encoding.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view offline_semantic_batch_source_sha256 =
    "0eca10ddb9a44e3ff3d179c6059c648d6a55871495ccb1fc2961913c3b706111";

class SourceAdapterUnavailable final : public std::invalid_argument {
public:
    using std::invalid_argument::invalid_argument;
};

struct SemanticPreparationFailure final {
    std::string source_id;
    std::string source_revision;
    std::string error_type;
    std::optional<std::string> response_utf8;
    std::int64_t elapsed_ns{};
    bool proposal_created{true};
    std::optional<std::string> failure_code;
    std::optional<std::string> finish_reason;
    std::size_t partial_unit_count{};

    SemanticPreparationFailure(
        std::string source_id, std::string source_revision,
        std::string error_type, std::optional<std::string> response_utf8,
        std::int64_t elapsed_ns, bool proposal_created = true,
        std::optional<std::string> failure_code = std::nullopt,
        std::optional<std::string> finish_reason = std::nullopt,
        std::size_t partial_unit_count = 0);

    [[nodiscard]] JsonValue::Object receipt() const;
    [[nodiscard]] static SemanticPreparationFailure from_receipt(
        const JsonValue::Object& row);
};

struct PreparedSemanticBatch final {
    std::vector<std::string> source_ids;
    std::vector<SemanticEncoding> proposals;
    std::vector<SemanticPreparationFailure> failures;
    std::int64_t elapsed_ns{};

    [[nodiscard]] std::vector<SemanticEncoding> for_wave(
        const std::vector<SemanticSourceEpisode>& episodes) const;
};

using SemanticPartsAdapter = std::function<std::vector<SemanticDeliveredPart>(
    const SemanticSourceEpisode&)>;

[[nodiscard]] PreparedSemanticBatch classify_partial_response_units(
    const PreparedSemanticBatch& batch);
[[nodiscard]] PreparedSemanticBatch retain_complete_length_units(
    const PreparedSemanticBatch& batch);
[[nodiscard]] PreparedSemanticBatch prepare_semantic_batch(
    const std::vector<SemanticSourceEpisode>& episodes,
    const SemanticPartsAdapter& parts_for,
    std::string model,
    const SemanticProducer& producer);

}  // namespace swegca::world
