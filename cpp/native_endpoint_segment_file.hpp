#pragma once

#include "event_vrs_inputs.hpp"
#include "owner_lock.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>

namespace swegca::vrs {

struct NativeEndpointSegment {
    std::filesystem::path path;
    std::string journal_generation;
    std::uint64_t first_edge = 0;
    std::uint64_t edge_count = 0;
};

// One immutable source-ordered dependency segment. Outgoing and incoming
// entries are stable-sorted by endpoint, with the original edge address as
// payload. The source's adjacent stable merge is streamed without loading an
// older large segment into RAM. The index owner applies the geometric rule.
// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_dependency_index.py@7536139:13-27
class NativeEndpointSegmentFile {
public:
    // The caller has already committed the source observation batch under
    // this Main owner lock. An orphaned unreferenced segment is derived data.
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_dependency_index.py@7536139:13-18
    [[nodiscard]] static NativeEndpointSegment create(
        const std::filesystem::path& directory,
        std::string journal_generation,
        std::uint64_t first_edge,
        std::span<const EventEdge> appended,
        OwnerLock& owner);

    // Adjacent chronological segments are stable-merged; equal endpoint keys
    // retain left-before-right edge order, as in the author implementation.
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_dependency_index.py@7536139:21-27
    [[nodiscard]] static NativeEndpointSegment merge(
        const std::filesystem::path& directory,
        const NativeEndpointSegment& left,
        const NativeEndpointSegment& right,
        OwnerLock& owner);

    // Check metadata, all entry checksums, sorted order, and one occurrence
    // of every edge address in both endpoint directions.
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_dependency_index.py@7536139:13-27
    static void validate_all(const NativeEndpointSegment& segment);

    // Cold recovery additionally binds every stored endpoint to the exact
    // immutable numerical source address. Checksums and a permutation alone
    // cannot prove that source and target columns were not exchanged.
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_dependency_index.py@7536139:46-53
    static void validate_source(
        const NativeEndpointSegment& segment,
        const EventVrsInputView& source);

    // Binary search the endpoint column, then stream every exact edge address
    // in the segment's stable order.
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_dependency_index.py@7536139:55-68
    static void visit_edges(
        const NativeEndpointSegment& segment,
        std::uint32_t node, EndpointDirection direction,
        const std::function<void(std::uint32_t)>& visit);
};

}  // namespace swegca::vrs
