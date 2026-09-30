#pragma once

#include "world/vrs_array_blocks.hpp"
#include "world/vrs_event_signal.hpp"

#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace swegca::world {

inline constexpr std::string_view vrs_event_delta_source_sha256 =
    "4fe59c75cb81c2eca7e52ad9b864efa75547b6822a07c1942de1ada72e1b4334";

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

// Prepare changed score blocks for a settled event. The returned object is a
// detached storage candidate; it neither commits main nor stores strengths.
[[nodiscard]] VrsArrayBlocks::Patched prepare_signal_score_storage(
    const EventSignalInputs& parent_inputs,
    const EventSignalProposal& proposal,
    const std::shared_ptr<const VrsArrayBlocks>& parent_blocks,
    VrsBlockCodec codec = VrsBlockCodec::zlib);

}  // namespace swegca::world
