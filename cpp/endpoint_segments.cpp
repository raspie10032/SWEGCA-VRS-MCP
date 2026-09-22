#include "endpoint_segments.hpp"

#include <algorithm>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <utility>

namespace swegca::vrs {

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_dependency_index.py@7536139:40-44
SegmentedEndpointDependencyIndex::SegmentedEndpointDependencyIndex(
    const EventVrsInputView& source,
    std::vector<std::shared_ptr<const Segment>> segments)
    : source_(&source), segments_(std::move(segments)) {
    if (segments_.empty())
        throw std::runtime_error("dependency index requires source and base segment");
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_dependency_index.py@7536139:13-18
std::shared_ptr<const SegmentedEndpointDependencyIndex::Segment>
SegmentedEndpointDependencyIndex::make_segment(
    std::span<const EventEdge> edges, std::uint64_t offset) {
    if (offset + edges.size() > std::numeric_limits<std::uint32_t>::max())
        throw std::runtime_error("dependency edge address outside u32 directory");
    auto result = std::make_shared<Segment>();
    result->outgoing.reserve(edges.size());
    result->incoming.reserve(edges.size());
    for (std::size_t at = 0; at < edges.size(); ++at) {
        const auto edge = static_cast<std::uint32_t>(offset + at);
        result->outgoing.push_back(Entry{edges[at].source, edge});
        result->incoming.push_back(Entry{edges[at].target, edge});
    }
    const auto by_node = [](const Entry& left, const Entry& right) {
        return left.node < right.node;
    };
    std::stable_sort(result->outgoing.begin(), result->outgoing.end(), by_node);
    std::stable_sort(result->incoming.begin(), result->incoming.end(), by_node);
    return result;
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_dependency_index.py@7536139:21-27
std::shared_ptr<const SegmentedEndpointDependencyIndex::Segment>
SegmentedEndpointDependencyIndex::merge_segments(const Segment& left,
                                                   const Segment& right) {
    auto result = std::make_shared<Segment>();
    const auto by_node = [](const Entry& lhs, const Entry& rhs) {
        return lhs.node < rhs.node;
    };
    result->outgoing.reserve(left.outgoing.size() + right.outgoing.size());
    result->incoming.reserve(left.incoming.size() + right.incoming.size());
    std::merge(left.outgoing.begin(), left.outgoing.end(),
               right.outgoing.begin(), right.outgoing.end(),
               std::back_inserter(result->outgoing), by_node);
    std::merge(left.incoming.begin(), left.incoming.end(),
               right.incoming.begin(), right.incoming.end(),
               std::back_inserter(result->incoming), by_node);
    return result;
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_dependency_index.py@7536139:46-49
std::shared_ptr<const SegmentedEndpointDependencyIndex>
SegmentedEndpointDependencyIndex::build_empty(
    const EventVrsInputView& source) {
    if (source.edge_count() != 0)
        throw std::runtime_error("empty dependency base requires zero edges");
    return std::shared_ptr<const SegmentedEndpointDependencyIndex>(
        new SegmentedEndpointDependencyIndex(source,
            {std::make_shared<const Segment>()}));
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_dependency_index.py@7536139:51-53
void SegmentedEndpointDependencyIndex::require_source(
    const EventVrsInputView& source) const {
    if (source_ != &source)
        throw std::runtime_error("dependency index belongs to a different immutable generation");
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_dependency_index.py@7536139:55-68
void SegmentedEndpointDependencyIndex::visit_edges(
    std::uint32_t node, EndpointDirection direction,
    const std::function<void(std::uint32_t)>& visit) const {
    for (const auto& segment : segments_) {
        const auto& entries = direction == EndpointDirection::outgoing ?
            segment->outgoing : segment->incoming;
        const auto lower = std::lower_bound(entries.begin(), entries.end(), node,
            [](const Entry& item, std::uint32_t probe) { return item.node < probe; });
        const auto upper = std::upper_bound(lower, entries.end(), node,
            [](std::uint32_t probe, const Entry& item) { return probe < item.node; });
        for (auto at = lower; at != upper; ++at) visit(at->edge);
    }
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_dependency_index.py@7536139:70-72
std::uint64_t SegmentedEndpointDependencyIndex::index_bytes() const {
    std::uint64_t bytes = 0;
    for (const auto& segment : segments_)
        bytes += (segment->outgoing.size() + segment->incoming.size()) *
                 (sizeof(std::uint32_t) + sizeof(std::uint32_t));
    return bytes;
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_delta.py@7536139:162-169
std::shared_ptr<const SegmentedEndpointDependencyIndex>
SegmentedEndpointDependencyIndex::extend_verified(
    const EventVrsInputView& successor,
    std::span<const EventEdge> appended) const {
    if (successor.edge_count() != source_->edge_count() + appended.size())
        throw std::runtime_error("successor edge count changed");
    auto segments = segments_;
    if (!appended.empty()) {
        segments.push_back(make_segment(appended, source_->edge_count()));
        while (segments.size() > 2 &&
               2 * segments.back()->outgoing.size() >=
                   segments[segments.size() - 2]->outgoing.size()) {
            const auto merged = merge_segments(*segments[segments.size() - 2],
                                               *segments.back());
            segments.pop_back();
            segments.back() = merged;
        }
    }
    return std::shared_ptr<const SegmentedEndpointDependencyIndex>(
        new SegmentedEndpointDependencyIndex(successor, std::move(segments)));
}

}  // namespace swegca::vrs
