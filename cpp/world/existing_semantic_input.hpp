#pragma once

#include "world/memory_activation.hpp"
#include "world/offline_semantic_batch.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view existing_semantic_input_source_sha256 =
    "8e2ad8e739c5dcf817c6c62e06d1e07626da927ae94fade2be6cae06491b60f8";

struct ExistingSemanticInput final {
    std::vector<SemanticSourceEpisode> episodes;
    PreparedSemanticBatch batch;
    JsonValue payload;
    std::string canonical_payload;
    std::string sha256;
    [[nodiscard]] constexpr std::size_t row_count() const noexcept { return 0; }
    [[nodiscard]] constexpr std::string_view input_kind() const noexcept {
        return "existing_source_semantics";
    }
};

[[nodiscard]] SemanticSourceEpisode semantic_source_episode(
    const MemoryEpisode& episode);
[[nodiscard]] ExistingSemanticInput bind_existing_semantics(
    const HotMemoryIndex& memory, const PreparedSemanticBatch& batch);
[[nodiscard]] ExistingSemanticInput restore_existing_semantic_input(
    const JsonValue& payload, const HotMemoryIndex& memory);

}  // namespace swegca::world
