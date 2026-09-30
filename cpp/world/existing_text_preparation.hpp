#pragma once

#include "world/offline_semantic_batch.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view existing_text_preparation_source_sha256 =
    "ae439da6067891fcf95886de6583e48be93ee8c348b8306e645a68f7cc95c845";
inline constexpr std::string_view existing_text_document_schema =
    "rozephine-genshin-namuwiki-character-assimilation-v1";
inline constexpr std::string_view adjacent_text_context_schema =
    "rozephine-adjacent-text-context-v1";
extern const std::string adjacent_text_context_prompt;

struct PreparedExistingText final {
    std::vector<PreparedSemanticBatch> chunks;
    std::vector<std::string> pending_anchors;
    PreparedSemanticBatch batch;
    std::size_t calls_this_slice{};
    std::size_t window_count{};
    std::vector<SemanticDeliveredPart> context_parts;
    std::size_t window_context_characters{};
    std::vector<std::string> contextual_windows;

    [[nodiscard]] JsonValue receipt() const;
};

[[nodiscard]] std::vector<SemanticDeliveredPart> archived_document_parts(
    const SemanticSourceEpisode& source);

[[nodiscard]] std::vector<PreparedSemanticBatch> restore_text_chunks(
    const JsonValue::Array& rows,
    const SemanticSourceEpisode& source);

[[nodiscard]] PreparedExistingText prepare_existing_text(
    const SemanticSourceEpisode& source,
    const SemanticPartsAdapter& parts_for,
    std::string model,
    const SemanticProducer& producer,
    std::size_t maximum_characters = 2048,
    std::size_t maximum_calls = 1,
    std::vector<PreparedSemanticBatch> completed = {},
    std::vector<SemanticDeliveredPart> context_parts = {},
    std::size_t window_context_characters = 0,
    std::vector<std::string> contextual_windows = {});

[[nodiscard]] PreparedExistingText restore_prepared_text(
    const JsonValue& receipt,
    const SemanticSourceEpisode& source,
    const SemanticPartsAdapter& parts_for,
    std::size_t maximum_characters);

}  // namespace swegca::world
