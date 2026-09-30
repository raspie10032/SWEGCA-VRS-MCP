#include "world/existing_session_semantic_input.hpp"

#include <stdexcept>

namespace swegca::world {

std::vector<std::string> PreparedSessionSemanticBatch::source_ids() const {
    if (!binding)
        throw std::invalid_argument("prepared_session_semantic_batch_required");
    std::vector<std::string> result;
    for (const auto& episode : binding->original_episodes)
        result.push_back(episode.episode_id);
    return result;
}

const std::vector<SemanticSourceEpisode>& ExistingSessionSemanticInput::episodes() const {
    if (!batch.binding)
        throw std::invalid_argument("prepared_session_semantic_batch_required");
    return batch.binding->original_episodes;
}

std::string_view ExistingSessionSemanticInput::payload() const {
    if (!batch.binding)
        throw std::invalid_argument("prepared_session_semantic_batch_required");
    return batch.binding->payload;
}

std::string_view ExistingSessionSemanticInput::sha256() const {
    if (!batch.binding)
        throw std::invalid_argument("prepared_session_semantic_batch_required");
    return batch.binding->sha256;
}

ExistingSessionSemanticInput restore_existing_session_input(
    const std::string_view payload, const HotMemoryIndex& memory,
    const PreparedSessionView& view,
    const std::vector<SemanticSourceEpisode>& source_episodes) {
    auto bound = restore_session_semantics_bytes(payload, view, source_episodes,
        memory.snapshot_id());
    if (!bound)
        throw std::invalid_argument("session_semantic_input_receipt_changed");
    if (memory.contains_episode(bound->derivative.episode_id))
        throw std::invalid_argument("session_semantic_derivative_already_accumulated");
    return {{std::move(bound)}};
}

ExistingSessionSemanticInput bind_existing_session_batch(
    const HotMemoryIndex& memory, const PreparedSessionSemanticBatch& batch,
    const PreparedSessionView& view,
    const std::vector<SemanticSourceEpisode>& source_episodes) {
    if (!batch.binding)
        throw std::invalid_argument("prepared_session_semantic_batch_required");
    return restore_existing_session_input(
        batch.binding->payload, memory, view, source_episodes);
}

}  // namespace swegca::world
