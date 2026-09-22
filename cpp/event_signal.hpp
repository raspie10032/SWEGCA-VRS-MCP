#pragma once

#include "event_vrs_kernel.hpp"
#include "vrs_state_update.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace swegca::vrs {

class EventSignalProposal final : public EventVrsProposal {
public:
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_signal.py@7536139:23-35
    [[nodiscard]] Json receipt() const override;
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_signal.py@7536139:23-35
    [[nodiscard]] const std::shared_ptr<const VRSStateUpdateReceipt>& strength_receipt() const {
        return strength_receipt_;
    }
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_signal.py@7536139:23-35
    [[nodiscard]] const std::string& strength_storage_dtype() const { return strength_storage_dtype_; }

private:
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_signal.py@7536139:181-185
    EventSignalProposal(
        std::shared_ptr<const ValidatedEventVrsInputs> inputs,
        std::map<std::uint32_t, float> scores,
        std::map<std::uint32_t, float> strengths,
        std::vector<std::uint32_t> pending_nodes,
        std::uint64_t rounds, std::uint64_t node_evaluations,
        std::uint64_t edge_evaluations,
        std::vector<std::uint32_t> seed_nodes,
        std::shared_ptr<const VRSStateUpdateReceipt> strength_receipt,
        std::string strength_storage_dtype);

    std::shared_ptr<const VRSStateUpdateReceipt> strength_receipt_;
    std::string strength_storage_dtype_;

    friend EventSignalProposal settle_event_signal(
        std::shared_ptr<const ValidatedEventVrsInputs> inputs,
        const std::vector<std::int64_t>& changed_nodes,
        std::shared_ptr<const VRSStateUpdateReceipt> strength_updates,
        std::string_view connection_namespace,
        const EventSignalProposal* previous,
        std::int64_t maximum_rounds,
        std::optional<std::string> strength_storage_dtype);
};

// Apply fixed Re-evidence strength once, then propagate signal. An exhausted
// round budget returns pending nodes; no durable publication happens here.
// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_signal.py@7536139:100-185
[[nodiscard]] EventSignalProposal settle_event_signal(
    std::shared_ptr<const ValidatedEventVrsInputs> inputs,
    const std::vector<std::int64_t>& changed_nodes = {},
    std::shared_ptr<const VRSStateUpdateReceipt> strength_updates = nullptr,
    std::string_view connection_namespace = "vrs-edge:",
    const EventSignalProposal* previous = nullptr,
    std::int64_t maximum_rounds = 1,
    std::optional<std::string> strength_storage_dtype = std::nullopt);

}  // namespace swegca::vrs
