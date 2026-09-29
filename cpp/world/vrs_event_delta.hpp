#pragma once

#include "world/vrs_event_signal.hpp"

#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace swegca::world {

inline constexpr std::string_view vrs_event_delta_source_sha256 =
    "ab60dccb5914d6eb01319c61094d1e43f1e883da180c4e815a7201f99cc9cfdd";

[[nodiscard]] std::shared_ptr<const EventSignalInputs> prepare_event_delta(
    const std::shared_ptr<const EventSignalInputs>& parent,
    std::string snapshot_id,
    std::span<const float> appended_direct,
    std::span<const float> appended_score,
    std::span<const std::uint8_t> appended_unresolved,
    std::span<const EventSignalEdge> appended_edges,
    std::span<const float> appended_strength,
    std::span<const std::size_t> base_indices = {},
    std::span<const float> base_values = {},
    std::span<const std::size_t> strength_indices = {},
    std::span<const float> strength_values = {},
    std::span<const std::size_t> direct_indices = {},
    std::span<const float> direct_values = {},
    std::span<const std::size_t> score_indices = {},
    std::span<const float> score_values = {},
    std::span<const std::size_t> unresolved_indices = {},
    std::span<const std::uint8_t> unresolved_values = {});

}  // namespace swegca::world
