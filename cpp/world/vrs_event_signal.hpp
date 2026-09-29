#pragma once

#include "world/detached_vrs_state_update.hpp"
#include "world/persistent_event_vector.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view vrs_event_signal_source_sha256 =
    "f851084f095a539a8098ce3f94a366a7ec91defcc040b7c2ce9db5ba305a51b1";
inline constexpr std::string_view vrs_event_signal_version =
    "vrs-re-evidence-event-signal-f32-v2-experimental";

struct EventSignalEdge final {
    std::uint32_t source{};
    std::uint32_t target{};
    std::int8_t sign{};
    float vrs_strength{};
    friend bool operator==(const EventSignalEdge&, const EventSignalEdge&) = default;
};

enum class EventStrengthStorage : std::uint8_t {
    float32,
    float16,
};

// One binary16 conversion boundary is shared by event arithmetic and its
// detached storage candidate. It carries no verdict or publication authority.
[[nodiscard]] std::uint16_t event_strength_float16_bits(float value);
[[nodiscard]] float event_strength_from_float16_bits(std::uint16_t value);

class EventSignalInputs final {
public:
    EventSignalInputs(std::string snapshot_id, std::vector<float> direct,
                      std::vector<float> score, std::vector<EventSignalEdge> edges,
                      std::vector<float> strength,
                      std::vector<std::uint8_t> unresolved);

    EventSignalInputs(const EventSignalInputs&) = delete;
    EventSignalInputs& operator=(const EventSignalInputs&) = delete;
    EventSignalInputs(EventSignalInputs&&) = delete;
    EventSignalInputs& operator=(EventSignalInputs&&) = delete;

    const std::string snapshot_id;
    const PersistentEventVector<float> direct;
    const PersistentEventVector<float> score;
    const PersistentEventVector<EventSignalEdge> edges;
    const PersistentEventVector<float> strength;
    const PersistentEventVector<std::uint8_t> unresolved;

    [[nodiscard]] std::span<const std::size_t> incoming(std::size_t node) const;
    [[nodiscard]] std::span<const std::size_t> outgoing(std::size_t node) const;

private:
    friend std::shared_ptr<const EventSignalInputs> prepare_event_delta(
        const std::shared_ptr<const EventSignalInputs>&, std::string,
        std::span<const float>, std::span<const float>,
        std::span<const std::uint8_t>, std::span<const EventSignalEdge>,
        std::span<const float>, std::span<const std::size_t>,
        std::span<const float>, std::span<const std::size_t>,
        std::span<const float>, std::span<const std::size_t>,
        std::span<const float>, std::span<const std::size_t>,
        std::span<const float>, std::span<const std::size_t>,
        std::span<const std::uint8_t>);
    EventSignalInputs(
        std::string snapshot_id, PersistentEventVector<float> direct,
        PersistentEventVector<float> score,
        PersistentEventVector<EventSignalEdge> edges,
        PersistentEventVector<float> strength,
        PersistentEventVector<std::uint8_t> unresolved,
        std::shared_ptr<const EventSignalInputs> delta_parent,
        std::vector<std::size_t> score_indices,
        std::vector<std::size_t> strength_indices,
        std::vector<std::size_t> direct_indices);
    std::vector<std::vector<std::size_t>> incoming_;
    std::vector<std::vector<std::size_t>> outgoing_;
    std::shared_ptr<const EventSignalInputs> delta_parent_;
    std::map<std::size_t, std::vector<std::size_t>> incoming_delta_;
    std::map<std::size_t, std::vector<std::size_t>> outgoing_delta_;
    std::vector<std::size_t> score_indices_;
    std::vector<std::size_t> strength_indices_;
    std::vector<std::size_t> direct_indices_;

public:
    [[nodiscard]] const EventSignalInputs* delta_parent() const noexcept { return delta_parent_.get(); }
    [[nodiscard]] std::span<const std::size_t> score_indices() const noexcept { return score_indices_; }
    [[nodiscard]] std::span<const std::size_t> strength_indices() const noexcept { return strength_indices_; }
    [[nodiscard]] std::span<const std::size_t> direct_indices() const noexcept { return direct_indices_; }
};

class EventSignalProposal final {
public:
    EventSignalProposal(const EventSignalProposal&) = default;
    EventSignalProposal(EventSignalProposal&&) noexcept = default;
    EventSignalProposal& operator=(const EventSignalProposal&) = delete;
    EventSignalProposal& operator=(EventSignalProposal&&) = delete;

    [[nodiscard]] bool pending() const noexcept { return !pending_nodes.empty(); }
    [[nodiscard]] bool authority_granted() const noexcept { return false; }
    [[nodiscard]] bool persistent_state_mutated() const noexcept { return false; }
    [[nodiscard]] bool whole_graph_convergence_claimed() const noexcept { return false; }
    [[nodiscard]] bool cognitive_completion() const noexcept { return false; }
    [[nodiscard]] bool belongs_to(const EventSignalInputs& inputs) const noexcept {
        return inputs_ == &inputs;
    }
    [[nodiscard]] std::size_t input_strength_proposal_count() const noexcept {
        return strength_receipt ? strength_receipt->updates.size() : 0;
    }

    const std::map<std::size_t, float> scores;
    const std::map<std::size_t, float> strengths;
    const std::vector<std::size_t> pending_nodes;
    const std::uint64_t rounds;
    const std::uint64_t node_evaluations;
    const std::uint64_t edge_evaluations;
    const std::vector<std::size_t> seed_nodes;
    const std::shared_ptr<const DetachedVrsStateUpdateReceipt> strength_receipt;
    const EventStrengthStorage strength_storage;

private:
    friend EventSignalProposal settle_event_signal(
        const EventSignalInputs&, std::span<const std::size_t>,
        std::shared_ptr<const DetachedVrsStateUpdateReceipt>, std::string_view,
        const EventSignalProposal*, std::uint64_t, std::optional<EventStrengthStorage>);
    EventSignalProposal(
        const EventSignalInputs* inputs, std::map<std::size_t, float> scores,
        std::map<std::size_t, float> strengths,
        std::vector<std::size_t> pending_nodes, std::uint64_t rounds,
        std::uint64_t node_evaluations, std::uint64_t edge_evaluations,
        std::vector<std::size_t> seed_nodes,
        std::shared_ptr<const DetachedVrsStateUpdateReceipt> strength_receipt,
        EventStrengthStorage strength_storage);

    const EventSignalInputs* const inputs_;
};

// The Re-evidence receipt is bound once at event ingress. Numerical rounds use
// fixed strengths and cannot create another evidence update or durable write.
[[nodiscard]] EventSignalProposal settle_event_signal(
    const EventSignalInputs& inputs,
    std::span<const std::size_t> changed_nodes = {},
    std::shared_ptr<const DetachedVrsStateUpdateReceipt> strength_updates = {},
    std::string_view connection_namespace = "vrs-edge:",
    const EventSignalProposal* previous = nullptr,
    std::uint64_t maximum_rounds = 1,
    std::optional<EventStrengthStorage> strength_storage = std::nullopt);

}  // namespace swegca::world
