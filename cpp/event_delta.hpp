#pragma once

#include "endpoint_segments.hpp"
#include "event_vrs_inputs.hpp"
#include "sparse_event_radix.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace swegca::vrs {

struct EventDeltaChanges {
    std::vector<float> appended_direct;
    std::vector<float> appended_score;
    std::vector<std::uint8_t> appended_unresolved;
    std::vector<EventEdge> appended_edges;
    std::vector<float> appended_strength;
    std::vector<std::pair<std::uint32_t, float>> base_edits;
    std::vector<std::pair<std::uint32_t, float>> strength_edits;
    std::vector<std::pair<std::uint32_t, float>> direct_edits;
    std::vector<std::pair<std::uint32_t, float>> score_edits;
    std::vector<std::pair<std::uint32_t, std::uint8_t>> unresolved_edits;
};

// Immutable numeric successor. The cold base is shared; sparse path copies
// preserve prior values and never change old edge endpoints or signs.
class EventDeltaView final : public EventVrsInputView {
public:
    // SWEGCA: src/swegca_vrs2/store.py@7536139:185-191
    [[nodiscard]] static std::shared_ptr<const ValidatedEventVrsInputs> empty(
        std::string snapshot_id);
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_delta.py@7536139:117-180
    [[nodiscard]] static std::shared_ptr<const ValidatedEventVrsInputs> prepare(
        std::shared_ptr<const ValidatedEventVrsInputs> parent,
        std::string snapshot_id, const EventDeltaChanges& changes);

    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_delta.py@7536139:170-175
    [[nodiscard]] std::string snapshot_id() const override { return snapshot_id_; }
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_delta.py@7536139:141-147
    [[nodiscard]] std::uint64_t node_count() const override { return node_count_; }
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_delta.py@7536139:145-147
    [[nodiscard]] std::uint64_t edge_count() const override { return edge_count_; }
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_delta.py@7536139:141-144
    [[nodiscard]] float direct(std::uint32_t node) const override;
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_delta.py@7536139:141-144
    [[nodiscard]] float score(std::uint32_t node) const override;
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_delta.py@7536139:141-144
    [[nodiscard]] bool unresolved(std::uint32_t node) const override;
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_delta.py@7536139:157-161
    [[nodiscard]] EventEdge edge(std::uint32_t address) const override;
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_delta.py@7536139:144-144
    [[nodiscard]] float strength(std::uint32_t address) const override;
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_delta.py@7536139:75-79
    void require_immutable_binding() const override;
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_delta.py@7536139:164-175
    [[nodiscard]] const EndpointDependencyIndex& dependencies() const override;

private:
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_delta.py@7536139:170-180
    EventDeltaView() = default;

    std::shared_ptr<const EventVrsInputView> base_;
    std::shared_ptr<const ValidatedEventVrsInputs> delta_parent_;
    std::string snapshot_id_;
    std::uint64_t node_count_ = 0;
    std::uint64_t edge_count_ = 0;
    SparseEventRadix<float> direct_;
    SparseEventRadix<float> score_;
    SparseEventRadix<std::uint8_t> unresolved_;
    SparseEventRadix<EventEdge> edge_;
    SparseEventRadix<float> strength_;
    std::shared_ptr<const SegmentedEndpointDependencyIndex> dependencies_;
};

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_delta.py@7536139:117-180
[[nodiscard]] std::shared_ptr<const ValidatedEventVrsInputs> prepare_event_delta(
    std::shared_ptr<const ValidatedEventVrsInputs> parent,
    std::string snapshot_id, const EventDeltaChanges& changes);

}  // namespace swegca::vrs
