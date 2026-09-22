#pragma once

#include "event_vrs_inputs.hpp"
#include "json.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <vector>

namespace swegca::vrs {

class EventVrsProposal {
public:
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:89-110
    virtual ~EventVrsProposal() = default;
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:112-120
    [[nodiscard]] virtual Json receipt() const;

    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:94-110
    [[nodiscard]] const std::map<std::uint32_t, float>& scores() const { return scores_; }
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:94-110
    [[nodiscard]] const std::map<std::uint32_t, float>& strengths() const { return strengths_; }
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:94-110
    [[nodiscard]] const std::vector<std::uint32_t>& pending_nodes() const { return pending_nodes_; }
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:94-110
    [[nodiscard]] const std::vector<std::uint32_t>& seed_nodes() const { return seed_nodes_; }
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:94-110
    [[nodiscard]] std::uint64_t rounds() const { return rounds_; }
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:94-110
    [[nodiscard]] std::uint64_t node_evaluations() const { return node_evaluations_; }
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:94-110
    [[nodiscard]] std::uint64_t edge_evaluations() const { return edge_evaluations_; }

protected:
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:101-110
    EventVrsProposal(std::shared_ptr<const ValidatedEventVrsInputs> inputs,
                     std::map<std::uint32_t, float> scores,
                     std::map<std::uint32_t, float> strengths,
                     std::vector<std::uint32_t> pending_nodes,
                     std::uint64_t rounds, std::uint64_t node_evaluations,
                     std::uint64_t edge_evaluations,
                     std::vector<std::uint32_t> seed_nodes);

    std::shared_ptr<const ValidatedEventVrsInputs> inputs_;
    std::map<std::uint32_t, float> scores_;
    std::map<std::uint32_t, float> strengths_;
    std::vector<std::uint32_t> pending_nodes_;
    std::uint64_t rounds_;
    std::uint64_t node_evaluations_;
    std::uint64_t edge_evaluations_;
    std::vector<std::uint32_t> seed_nodes_;

private:
    friend EventVrsProposal advance_event_vrs(
        std::shared_ptr<const ValidatedEventVrsInputs> inputs,
        const std::vector<std::int64_t>& changed_nodes,
        const EventVrsProposal* previous, std::int64_t maximum_rounds);
};

// A proposal is detached numerical work. Pending nodes remain pending when
// the round budget expires; Main alone decides whether it can publish.
// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:134-209
[[nodiscard]] EventVrsProposal advance_event_vrs(
    std::shared_ptr<const ValidatedEventVrsInputs> inputs,
    const std::vector<std::int64_t>& changed_nodes = {},
    const EventVrsProposal* previous = nullptr,
    std::int64_t maximum_rounds = 1);

}  // namespace swegca::vrs
