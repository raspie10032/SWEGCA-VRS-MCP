#pragma once

#include "event_vrs_inputs.hpp"
#include "native_endpoint_segment_file.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace swegca::vrs {

// Disk-backed form of the author's immutable endpoint dependency index. The
// cold base boundary is explicit so only chronological delta segments enter
// the geometric compaction rule. Each pinned instance belongs to one exact
// EventVrsInputView object; its immutable files may be shared by successors.
// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_dependency_index.py@7536139:30-72
class NativeEndpointDependencyIndex final : public EndpointDependencyIndex {
public:
    // Main begins with the author's empty Graph and an empty cold base.
    // Supplying a writer allows EventDeltaView to prepare file-backed
    // successors while the Main owner lock is held.
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_dependency_index.py@7536139:46-49
    [[nodiscard]] static std::shared_ptr<const NativeEndpointDependencyIndex>
    build_empty(const EventVrsInputView& source,
                std::filesystem::path directory,
                std::string journal_generation,
                OwnerLock* writer = nullptr);

    // Cold recovery accepts only a contiguous complete partition of source
    // edge addresses. Every segment is validated against the exact source.
    // base_edge_count identifies the first cold segment (zero means the
    // author's empty base sentinel, which has no file).
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_dependency_index.py@7536139:46-53
    [[nodiscard]] static std::shared_ptr<const NativeEndpointDependencyIndex>
    open_pinned(const EventVrsInputView& source,
                std::filesystem::path directory,
                std::string journal_generation,
                std::uint64_t base_edge_count,
                std::vector<NativeEndpointSegment> segments,
                OwnerLock* writer = nullptr);

    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_dependency_index.py@7536139:51-53
    void require_source(const EventVrsInputView& source) const override;
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_dependency_index.py@7536139:55-68
    void visit_edges(std::uint32_t node, EndpointDirection direction,
                     const std::function<void(std::uint32_t)>& visit) const override;

    // Metadata is exposed for the separate source-bound publication manifest;
    // file paths themselves confer no authority.
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_dependency_index.py@7536139:40-49
    [[nodiscard]] const std::vector<NativeEndpointSegment>& segments() const {
        return segments_;
    }
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_dependency_index.py@7536139:40-49
    [[nodiscard]] std::uint64_t base_edge_count() const {
        return base_edge_count_;
    }
    // SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:91-127
    [[nodiscard]] const std::filesystem::path& directory() const {
        return directory_;
    }
    // SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:91-127
    [[nodiscard]] const std::string& journal_generation() const {
        return journal_generation_;
    }
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_dependency_index.py@7536139:70-72
    [[nodiscard]] std::uint64_t index_bytes() const;

private:
    NativeEndpointDependencyIndex(
        const EventVrsInputView& source,
        std::filesystem::path directory,
        std::string journal_generation,
        std::uint64_t base_edge_count,
        std::vector<NativeEndpointSegment> segments,
        OwnerLock* writer);

    // EventDeltaView has already verified the immutable endpoint/sign prefix.
    // Only the appended suffix receives a new physical segment.
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_dependency_index.py@7536139:74-86
    [[nodiscard]] std::shared_ptr<const EndpointDependencyIndex> extend_verified(
        const EventVrsInputView& successor,
        std::span<const EventEdge> appended) const override;

    const EventVrsInputView* source_;
    std::filesystem::path directory_;
    std::string journal_generation_;
    std::uint64_t base_edge_count_ = 0;
    std::vector<NativeEndpointSegment> segments_;
    OwnerLock* writer_ = nullptr;
};

}  // namespace swegca::vrs
