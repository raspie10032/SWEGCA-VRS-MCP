#pragma once

#include "event_vrs_inputs.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace swegca::vrs {

class EventDeltaView;

// Source-ordered endpoint segments. A successor shares untouched segments;
// only the appended tail is constructed and geometrically merged.
// The current vector representation is a source reference, not the 4 GB
// product representation for a large main generation.
class SegmentedEndpointDependencyIndex final : public EndpointDependencyIndex {
public:
    // An empty Graph starts with one empty, generation-bound base segment.
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_dependency_index.py@7536139:46-49
    [[nodiscard]] static std::shared_ptr<const SegmentedEndpointDependencyIndex> build_empty(
        const EventVrsInputView& source);

    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_dependency_index.py@7536139:51-53
    void require_source(const EventVrsInputView& source) const override;
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_dependency_index.py@7536139:55-68
    void visit_edges(std::uint32_t node, EndpointDirection direction,
                     const std::function<void(std::uint32_t)>& visit) const override;
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_dependency_index.py@7536139:70-72
    [[nodiscard]] std::uint64_t index_bytes() const;

private:
    struct Entry {
        std::uint32_t node;
        std::uint32_t edge;
    };
    struct Segment {
        std::vector<Entry> outgoing;
        std::vector<Entry> incoming;
    };

    SegmentedEndpointDependencyIndex(
        const EventVrsInputView& source,
        std::vector<std::shared_ptr<const Segment>> segments);

    // EventDeltaView alone may call this: its construction preserves the old
    // endpoint/sign prefix by sharing the immutable parent, as in the author
    // prepare_event_delta path. Generic callers never bypass that check.
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_delta.py@7536139:162-169
    [[nodiscard]] std::shared_ptr<const EndpointDependencyIndex> extend_verified(
        const EventVrsInputView& successor,
        std::span<const EventEdge> appended) const override;

    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_dependency_index.py@7536139:13-18
    [[nodiscard]] static std::shared_ptr<const Segment> make_segment(
        std::span<const EventEdge> edges, std::uint64_t offset);
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_dependency_index.py@7536139:21-27
    [[nodiscard]] static std::shared_ptr<const Segment> merge_segments(
        const Segment& left, const Segment& right);

    // The generation owns this index, so retaining a shared pointer back to
    // the generation would form a reference cycle.
    const EventVrsInputView* source_;
    std::vector<std::shared_ptr<const Segment>> segments_;
    friend class EventDeltaView;
};

}  // namespace swegca::vrs
