#pragma once

#include "world/memory_activation.hpp"
#include "world/session_semantic_binding.hpp"

#include <memory>
#include <string_view>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view existing_session_semantic_input_source_sha256 =
    "a0d75a24618ab5cd7bc8b5801ff7fdb1621ebaa26509e16a07f34b4847e5f81e";

struct PreparedSessionSemanticBatch final {
    std::shared_ptr<const BoundSessionSemantics> binding;
    [[nodiscard]] std::vector<std::string> source_ids() const;
};

struct ExistingSessionSemanticInput final {
    PreparedSessionSemanticBatch batch;
    [[nodiscard]] constexpr std::size_t row_count() const noexcept { return 0; }
    [[nodiscard]] constexpr std::string_view input_kind() const noexcept {
        return "existing_session_semantics";
    }
    [[nodiscard]] const std::vector<SemanticSourceEpisode>& episodes() const;
    [[nodiscard]] std::string_view payload() const;
    [[nodiscard]] std::string_view sha256() const;
};

[[nodiscard]] ExistingSessionSemanticInput restore_existing_session_input(
    std::string_view payload,
    const HotMemoryIndex& memory,
    const PreparedSessionView& view,
    const std::vector<SemanticSourceEpisode>& source_episodes);
[[nodiscard]] ExistingSessionSemanticInput bind_existing_session_batch(
    const HotMemoryIndex& memory,
    const PreparedSessionSemanticBatch& batch,
    const PreparedSessionView& view,
    const std::vector<SemanticSourceEpisode>& source_episodes);

}  // namespace swegca::world
