#pragma once

#include "world/vrs_event_signal.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string_view>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view vrs_event_kernel_source_sha256 =
    "3bb83f4a052e46ad76eca44372a6d7c68fa23e7ba27c56c7f6319e558900f0b1";
inline constexpr std::string_view event_vrs_version =
    "vrs-event-synchronous-f32-v1-experimental";

struct EventVrsReceipt final {
    std::string_view version{event_vrs_version};
    std::string_view parent_snapshot_id;
    std::string_view status;
    std::size_t pending_node_count{};
    std::uint64_t rounds{};
    std::uint64_t node_evaluations{};
    std::uint64_t edge_evaluations{};
    std::size_t changed_scores{};
    std::size_t changed_strengths{};
    bool legacy_numerical_equivalence{};
    bool whole_graph_convergence_claimed{};
    bool logical_implication_claimed{};
    bool cognitive_completion{};
    bool persistent_state_mutated{};
    bool authority_granted{};
};

class EventVrsProposal final {
public:
    [[nodiscard]] EventVrsReceipt receipt() const noexcept;
    [[nodiscard]] bool belongs_to(const EventSignalInputs& inputs) const noexcept {
        return inputs_ == &inputs;
    }

    const std::map<std::size_t, float> scores;
    const std::map<std::size_t, float> strengths;
    const std::vector<std::size_t> pending_nodes;
    const std::uint64_t rounds;
    const std::uint64_t node_evaluations;
    const std::uint64_t edge_evaluations;
    const std::vector<std::size_t> seed_nodes;

private:
    friend EventVrsProposal advance_event_vrs(
        const EventSignalInputs&, std::span<const std::size_t>,
        const EventVrsProposal*, std::uint64_t);
    EventVrsProposal(
        const EventSignalInputs* inputs,
        std::map<std::size_t, float> scores,
        std::map<std::size_t, float> strengths,
        std::vector<std::size_t> pending_nodes,
        std::uint64_t rounds,
        std::uint64_t node_evaluations,
        std::uint64_t edge_evaluations,
        std::vector<std::size_t> seed_nodes);

    const EventSignalInputs* const inputs_;
};

// Detached, synchronous event-local arithmetic. The returned proposal is
// pending work owned by main and has no publication or authority capability.
[[nodiscard]] EventVrsProposal advance_event_vrs(
    const EventSignalInputs& inputs,
    std::span<const std::size_t> changed_nodes = {},
    const EventVrsProposal* previous = nullptr,
    std::uint64_t maximum_rounds = 1);

}  // namespace swegca::world
