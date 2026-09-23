#include "native_graph_numeric_view.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace swegca::vrs {
namespace {

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:44-45
bool sha256_id(std::string_view identifier) {
    return identifier.size() == 64 &&
        std::all_of(identifier.begin(), identifier.end(), [](char digit) {
            return (digit >= '0' && digit <= '9') ||
                   (digit >= 'a' && digit <= 'f');
        });
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_delta.py@7536139:141-163
std::uint64_t required_pages(std::uint64_t records) {
    return records == 0 ? 0 :
        (records - 1) / NativeGraphPageMap::records_per_page + 1;
}

}  // namespace

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:34-76
NativeGraphNumericView::NativeGraphNumericView(
    NativeGraphNumericPageState state,
    std::shared_ptr<const NativeGraphPageFile> node_file,
    std::shared_ptr<const NativeGraphPageFile> edge_file,
    std::shared_ptr<NativeGraphPageCache> page_cache)
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:34-76
    : state_(std::move(state)), node_file_(std::move(node_file)),
      edge_file_(std::move(edge_file)), page_cache_(std::move(page_cache)) {}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:34-76
std::shared_ptr<const ValidatedEventVrsInputs>
NativeGraphNumericView::open_validated(
    NativeGraphNumericPageState state,
    std::shared_ptr<const NativeGraphPageFile> node_file,
    std::shared_ptr<const NativeGraphPageFile> edge_file,
    std::filesystem::path endpoint_directory,
    std::uint64_t endpoint_base_edge_count,
    std::vector<NativeEndpointSegment> endpoint_segments,
    OwnerLock* writer,
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:34-76
    std::shared_ptr<NativeGraphPageCache> page_cache) {
    if (!node_file || !edge_file || endpoint_directory.empty() ||
        state.journal_generation != node_file->journal_generation() ||
        state.journal_generation != edge_file->journal_generation() ||
        state.node_count > std::uint64_t{0x100000000ULL} ||
        state.edge_count > std::numeric_limits<std::uint32_t>::max() ||
        state.node_pages.page_count() != required_pages(state.node_count) ||
        state.edge_pages.page_count() != required_pages(state.edge_count) ||
        !sha256_id(state.graph_snapshot_id))
        throw std::runtime_error("graph_numeric_view_generation_invalid");
    auto view = std::shared_ptr<NativeGraphNumericView>(
        new NativeGraphNumericView(
            std::move(state), std::move(node_file), std::move(edge_file),
            std::move(page_cache)));
    view->dependencies_ = NativeEndpointDependencyIndex::open_pinned(
        *view, std::move(endpoint_directory),
        view->state_.journal_generation, endpoint_base_edge_count,
        std::move(endpoint_segments), writer);
    return std::make_shared<const ValidatedEventVrsInputs>(
        std::shared_ptr<const EventVrsInputView>(std::move(view)));
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_delta.py@7536139:170-180
std::shared_ptr<const ValidatedEventVrsInputs>
NativeGraphNumericView::rebase_validated_successor(
    NativeGraphNumericPageState state,
    std::shared_ptr<const NativeGraphPageFile> node_file,
    std::shared_ptr<const NativeGraphPageFile> edge_file,
    const ValidatedEventVrsInputs& prepared_successor,
    OwnerLock* writer,
    std::shared_ptr<NativeGraphPageCache> page_cache) {
    const auto& prepared = prepared_successor.require_validated_immutable();
    const auto* endpoints = dynamic_cast<const NativeEndpointDependencyIndex*>(
        &prepared.dependencies());
    if (!endpoints || state.graph_snapshot_id != prepared.snapshot_id() ||
        state.node_count != prepared.node_count() ||
        state.edge_count != prepared.edge_count() ||
        state.journal_generation != endpoints->journal_generation())
        throw std::runtime_error("graph_numeric_rebase_source_changed");
    return open_validated(
        std::move(state), std::move(node_file), std::move(edge_file),
        endpoints->directory(), endpoints->base_edge_count(),
        endpoints->segments(), writer, std::move(page_cache));
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:136-194
std::shared_ptr<const ValidatedEventVrsInputs>
NativeGraphNumericView::open_recovered(
    const NativeGraphMapCursor& numeric,
    const NativeEndpointManifestCursor& endpoints,
    std::shared_ptr<const NativeGraphPageFile> node_file,
    std::shared_ptr<const NativeGraphPageFile> edge_file,
    OwnerLock* writer,
    std::shared_ptr<NativeGraphPageCache> page_cache) {
    if (numeric.pages.journal_generation != endpoints.journal_generation ||
        numeric.pages.graph_snapshot_id != endpoints.graph_snapshot_id ||
        numeric.pair_snapshot_id != endpoints.pair_snapshot_id ||
        numeric.pages.edge_count != endpoints.edge_count ||
        numeric.map_rows != endpoints.manifest_rows ||
        numeric.last_source_sequence != endpoints.last_source_sequence)
        throw std::runtime_error("graph_numeric_recovery_generations_disagree");
    return open_validated(
        numeric.pages, std::move(node_file), std::move(edge_file),
        endpoints.endpoint_directory, endpoints.base_edge_count,
        endpoints.segments, writer, std::move(page_cache));
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:34-76
GraphNumericNodeRecord NativeGraphNumericView::node_record(
    std::uint32_t node) const {
    if (node >= state_.node_count)
        throw std::out_of_range("graph_numeric_node_address_invalid");
    const auto page_id = node / NativeGraphPageMap::records_per_page;
    const auto physical = state_.node_pages.offset(page_id);
    if (!physical)
        throw std::runtime_error("graph_numeric_node_page_missing");
    const auto record = node % NativeGraphPageMap::records_per_page;
    if (page_cache_)
        return page_cache_->node_record(node_file_, *physical, page_id, record);
    auto page = node_file_->read_node(*physical, page_id);
    if (record >= page.valid_records)
        throw std::runtime_error("graph_numeric_node_page_changed");
    return page.records[record];
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:34-76
GraphNumericEdgeRecord NativeGraphNumericView::edge_record(
    std::uint32_t edge) const {
    if (edge >= state_.edge_count)
        throw std::out_of_range("graph_numeric_edge_address_invalid");
    const auto page_id = edge / NativeGraphPageMap::records_per_page;
    const auto physical = state_.edge_pages.offset(page_id);
    if (!physical)
        throw std::runtime_error("graph_numeric_edge_page_missing");
    const auto record = edge % NativeGraphPageMap::records_per_page;
    if (page_cache_)
        return page_cache_->edge_record(edge_file_, *physical, page_id, record);
    auto page = edge_file_->read_edge(*physical, page_id);
    if (record >= page.valid_records)
        throw std::runtime_error("graph_numeric_edge_page_changed");
    return page.records[record];
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:34-76
float NativeGraphNumericView::direct(std::uint32_t node) const {
    return node_record(node).direct;
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:34-76
float NativeGraphNumericView::score(std::uint32_t node) const {
    return node_record(node).score;
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:34-76
bool NativeGraphNumericView::unresolved(std::uint32_t node) const {
    return node_record(node).unresolved;
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:34-76
EventEdge NativeGraphNumericView::edge(std::uint32_t address) const {
    return edge_record(address).edge;
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:34-76
float NativeGraphNumericView::strength(std::uint32_t address) const {
    return edge_record(address).strength;
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:66-76
void NativeGraphNumericView::require_immutable_binding() const {
    if (!node_file_ || !edge_file_ || !dependencies_ ||
        state_.journal_generation != node_file_->journal_generation() ||
        state_.journal_generation != edge_file_->journal_generation() ||
        state_.node_pages.page_count() != required_pages(state_.node_count) ||
        state_.edge_pages.page_count() != required_pages(state_.edge_count) ||
        !sha256_id(state_.graph_snapshot_id))
        throw std::runtime_error("graph_numeric_view_generation_changed");
    dependencies_->require_source(*this);
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:34-76
const EndpointDependencyIndex& NativeGraphNumericView::dependencies() const {
    if (!dependencies_)
        throw std::runtime_error("graph_numeric_dependencies_missing");
    return *dependencies_;
}

}  // namespace swegca::vrs
