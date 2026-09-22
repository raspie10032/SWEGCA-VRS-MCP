#include "native_endpoint_index.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace swegca::vrs {
namespace {

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:91-127
bool valid_generation(std::string_view generation) {
    return generation.size() == 34 && generation.starts_with("g-") &&
        std::all_of(generation.begin() + 2, generation.end(), [](char digit) {
            return (digit >= '0' && digit <= '9') ||
                   (digit >= 'a' && digit <= 'f');
        });
}

// The source keeps a conceptual empty base segment. It is represented by
// base_edge_count==0 rather than an empty disk file.
// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_dependency_index.py@7536139:74-86
std::size_t tail_start(std::uint64_t base_edge_count) {
    return base_edge_count == 0 ? 0 : 1;
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_dependency_index.py@7536139:74-86
void require_geometric_tail(
    std::uint64_t base_edge_count,
    const std::vector<NativeEndpointSegment>& segments) {
    const auto first = tail_start(base_edge_count);
    for (std::size_t at = first + 1; at < segments.size(); ++at) {
        const auto previous = segments[at - 1].edge_count;
        const auto current = segments[at].edge_count;
        if (current >= previous / 2 + previous % 2)
            throw std::runtime_error("endpoint_index_geometric_order_invalid");
    }
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_dependency_index.py@7536139:46-53
void require_partition(
    const EventVrsInputView& source,
    const std::filesystem::path& directory,
    std::string_view generation,
    std::uint64_t base_edge_count,
    const std::vector<NativeEndpointSegment>& segments) {
    if (directory.empty() || !valid_generation(generation) ||
        source.edge_count() > std::numeric_limits<std::uint32_t>::max() ||
        base_edge_count > source.edge_count() ||
        (base_edge_count == 0 && source.edge_count() != 0 && segments.empty()) ||
        (base_edge_count != 0 && segments.empty()))
        throw std::runtime_error("endpoint_index_source_changed");
    std::uint64_t next = 0;
    for (std::size_t at = 0; at < segments.size(); ++at) {
        const auto& segment = segments[at];
        if (segment.path.parent_path() != directory ||
            segment.journal_generation != generation ||
            segment.first_edge != next || segment.edge_count == 0 ||
            segment.edge_count > std::numeric_limits<std::uint32_t>::max() ||
            (at == 0 && base_edge_count != 0 &&
             segment.edge_count != base_edge_count) ||
            next > std::numeric_limits<std::uint32_t>::max() -
                       segment.edge_count)
            throw std::runtime_error("endpoint_index_source_changed");
        NativeEndpointSegmentFile::validate_source(segment, source);
        next += segment.edge_count;
    }
    if (next != source.edge_count())
        throw std::runtime_error("endpoint_index_source_changed");
    require_geometric_tail(base_edge_count, segments);
}

}  // namespace

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_dependency_index.py@7536139:40-49
NativeEndpointDependencyIndex::NativeEndpointDependencyIndex(
    const EventVrsInputView& source,
    std::filesystem::path directory,
    std::string journal_generation,
    std::uint64_t base_edge_count,
    std::vector<NativeEndpointSegment> segments,
    OwnerLock* writer)
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_dependency_index.py@7536139:40-49
    : source_(&source), directory_(std::move(directory)),
      journal_generation_(std::move(journal_generation)),
      base_edge_count_(base_edge_count), segments_(std::move(segments)),
      writer_(writer) {}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_dependency_index.py@7536139:46-49
std::shared_ptr<const NativeEndpointDependencyIndex>
NativeEndpointDependencyIndex::build_empty(
    const EventVrsInputView& source,
    std::filesystem::path directory,
    std::string journal_generation,
    OwnerLock* writer) {
    if (source.edge_count() != 0 || directory.empty() ||
        !valid_generation(journal_generation) ||
        (writer && !writer->locked()))
        throw std::runtime_error("empty endpoint index source changed");
    return std::shared_ptr<const NativeEndpointDependencyIndex>(
        new NativeEndpointDependencyIndex(
            source, std::move(directory), std::move(journal_generation),
            0, {}, writer));
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_dependency_index.py@7536139:46-53
std::shared_ptr<const NativeEndpointDependencyIndex>
NativeEndpointDependencyIndex::open_pinned(
    const EventVrsInputView& source,
    std::filesystem::path directory,
    std::string journal_generation,
    std::uint64_t base_edge_count,
    std::vector<NativeEndpointSegment> segments,
    OwnerLock* writer) {
    if (writer && !writer->locked())
        throw std::runtime_error("native_vrs_owner_lock_required");
    require_partition(source, directory, journal_generation,
                      base_edge_count, segments);
    return std::shared_ptr<const NativeEndpointDependencyIndex>(
        new NativeEndpointDependencyIndex(
            source, std::move(directory), std::move(journal_generation),
            base_edge_count, std::move(segments), writer));
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_dependency_index.py@7536139:51-53
void NativeEndpointDependencyIndex::require_source(
    const EventVrsInputView& source) const {
    if (source_ != &source)
        throw std::runtime_error(
            "dependency index belongs to a different immutable generation");
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_dependency_index.py@7536139:55-68
void NativeEndpointDependencyIndex::visit_edges(
    std::uint32_t node, EndpointDirection direction,
    const std::function<void(std::uint32_t)>& visit) const {
    if (!visit) throw std::runtime_error("endpoint_segment_visitor_missing");
    for (const auto& segment : segments_)
        NativeEndpointSegmentFile::visit_edges(
            segment, node, direction, visit);
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_dependency_index.py@7536139:70-72
std::uint64_t NativeEndpointDependencyIndex::index_bytes() const {
    std::uint64_t total = 0;
    for (const auto& segment : segments_) {
        constexpr std::uint64_t header_bytes = 96;
        constexpr std::uint64_t bytes_per_edge = 24;
        const auto bytes = header_bytes + bytes_per_edge * segment.edge_count;
        if (total > std::numeric_limits<std::uint64_t>::max() - bytes)
            throw std::runtime_error("endpoint_index_size_overflow");
        total += bytes;
    }
    return total;
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_dependency_index.py@7536139:74-86
std::shared_ptr<const EndpointDependencyIndex>
NativeEndpointDependencyIndex::extend_verified(
    const EventVrsInputView& successor,
    std::span<const EventEdge> appended) const {
    if (!writer_ || !writer_->locked())
        throw std::runtime_error("native_vrs_owner_lock_required");
    if (appended.size() > std::numeric_limits<std::uint32_t>::max() ||
        source_->edge_count() >
            std::numeric_limits<std::uint32_t>::max() - appended.size() ||
        successor.edge_count() != source_->edge_count() + appended.size())
        throw std::runtime_error("successor edge count changed");
    auto segments = segments_;
    if (!appended.empty()) {
        segments.push_back(NativeEndpointSegmentFile::create(
            directory_, journal_generation_, source_->edge_count(),
            appended, *writer_));
        const auto first = tail_start(base_edge_count_);
        while (segments.size() - first >= 2) {
            const auto& previous = segments[segments.size() - 2];
            const auto& current = segments.back();
            if (current.edge_count <
                previous.edge_count / 2 + previous.edge_count % 2)
                break;
            auto merged = NativeEndpointSegmentFile::merge(
                directory_, previous, current, *writer_);
            segments.pop_back();
            segments.back() = std::move(merged);
        }
    }
    return std::shared_ptr<const NativeEndpointDependencyIndex>(
        new NativeEndpointDependencyIndex(
            successor, directory_, journal_generation_, base_edge_count_,
            std::move(segments), writer_));
}

}  // namespace swegca::vrs
