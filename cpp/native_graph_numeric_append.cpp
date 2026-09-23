#include "native_graph_numeric_append.hpp"

#include "main_journal_append.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

namespace swegca::vrs {
namespace {

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_delta.py@7536139:141-163
std::uint64_t required_pages(std::uint64_t records) {
    return records == 0 ? 0 :
        (records - 1) / NativeGraphPageMap::records_per_page + 1;
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_delta.py@7536139:141-163
void add_appended_pages(std::vector<std::uint32_t>& pages,
                        std::uint64_t old_count,
                        std::uint64_t new_count) {
    if (new_count < old_count || new_count > std::uint64_t{0x100000000ULL})
        throw std::runtime_error("graph_numeric_append_count_invalid");
    if (new_count == old_count) return;
    const auto first = old_count / NativeGraphPageMap::records_per_page;
    const auto last = (new_count - 1) / NativeGraphPageMap::records_per_page;
    for (auto page = first; page <= last; ++page)
        pages.push_back(static_cast<std::uint32_t>(page));
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_delta.py@7536139:59-66
void finish_pages(std::vector<std::uint32_t>& pages) {
    std::sort(pages.begin(), pages.end());
    pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_delta.py@7536139:84-94
std::uint32_t live_in_page(std::uint64_t count, std::uint32_t page_id) {
    const auto start = std::uint64_t(page_id) *
        NativeGraphPageMap::records_per_page;
    if (count <= start) return 0;
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(
        NativeGraphPageMap::records_per_page, count - start));
}

// The old physical page is compared to its cold-bound numerical source before
// copying it into a new generation. Retrieval rank or a stored checksum alone
// cannot authorize a changed value.
// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:34-76
void require_old_node_page(const NativeGraphNodePage& page,
                           const EventVrsInputView& parent,
                           std::uint32_t expected_count) {
    if (page.valid_records != expected_count)
        throw std::runtime_error("graph_numeric_parent_page_changed");
    const auto start = std::uint64_t(page.page_id) *
        NativeGraphPageMap::records_per_page;
    for (std::uint32_t at = 0; at < expected_count; ++at) {
        const auto address = static_cast<std::uint32_t>(start + at);
        const auto& old = page.records[at];
        if (std::bit_cast<std::uint32_t>(old.direct) !=
                std::bit_cast<std::uint32_t>(parent.direct(address)) ||
            std::bit_cast<std::uint32_t>(old.score) !=
                std::bit_cast<std::uint32_t>(parent.score(address)) ||
            old.unresolved != parent.unresolved(address))
            throw std::runtime_error("graph_numeric_parent_page_changed");
    }
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:34-76
void require_old_edge_page(const NativeGraphEdgePage& page,
                           const EventVrsInputView& parent,
                           std::uint32_t expected_count) {
    if (page.valid_records != expected_count)
        throw std::runtime_error("graph_numeric_parent_page_changed");
    const auto start = std::uint64_t(page.page_id) *
        NativeGraphPageMap::records_per_page;
    for (std::uint32_t at = 0; at < expected_count; ++at) {
        const auto address = static_cast<std::uint32_t>(start + at);
        const auto& old = page.records[at];
        const auto source = parent.edge(address);
        if (old.edge.source != source.source ||
            old.edge.target != source.target ||
            old.edge.sign != source.sign ||
            std::bit_cast<std::uint32_t>(old.edge.vrs_strength) !=
                std::bit_cast<std::uint32_t>(source.vrs_strength) ||
            std::bit_cast<std::uint32_t>(old.strength) !=
                std::bit_cast<std::uint32_t>(parent.strength(address)))
            throw std::runtime_error("graph_numeric_parent_page_changed");
    }
}

}  // namespace

// SWEGCA: src/swegca_vrs2/store.py@7536139:384-399
// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_delta.py@7536139:117-180
NativeGraphNumericPageResult append_committed_graph_numeric_pages(
    const NativeJournal& journal, const JournalAppendResult& committed,
    const MainObservationBatchPlan& batch,
    std::shared_ptr<const ValidatedEventVrsInputs> parent,
    const NativeGraphNumericPageState& parent_pages,
    std::string_view published_parent_pair,
    NativeGraphPageFile& node_file, NativeGraphPageFile& edge_file) {
    if (!parent ||
        parent_pages.journal_generation != journal.generation() ||
        node_file.journal_generation() != journal.generation() ||
        edge_file.journal_generation() != journal.generation())
        throw std::runtime_error("graph_numeric_batch_source_changed");
    require_committed_main_observation_frame(
        journal, committed, batch, published_parent_pair);
    const auto& old = parent->require_validated_immutable();
    if (parent_pages.graph_snapshot_id != old.snapshot_id() ||
        parent_pages.node_count != old.node_count() ||
        parent_pages.edge_count != old.edge_count() ||
        parent_pages.node_pages.page_count() != required_pages(old.node_count()) ||
        parent_pages.edge_pages.page_count() != required_pages(old.edge_count()))
        throw std::runtime_error("graph_numeric_batch_source_changed");

    std::vector<std::uint32_t> node_ids;
    std::vector<std::uint32_t> edge_ids;
    std::uint64_t node_count = old.node_count();
    std::uint64_t edge_count = old.edge_count();
    std::string graph_snapshot = old.snapshot_id();
    auto successor = parent;
    for (const auto& transition : batch.graph_transitions) {
        const auto& graph = transition.append;
        if (!transition.numerical.settled ||
            transition.journal_row_index >= batch.journal_rows.size() ||
            graph.parent_snapshot_id != graph_snapshot ||
            transition.numerical.event_snapshot_id != graph.snapshot_id ||
            transition.pair_snapshot_id != batch.journal_rows[
                transition.journal_row_index].pair_id)
            throw std::runtime_error("graph_numeric_batch_source_changed");
        const auto& current =
            transition.numerical.settled->require_validated_immutable();
        if (current.node_count() != node_count + graph.new_nodes.size() ||
            current.edge_count() != edge_count + graph.appended_edges.size())
            throw std::runtime_error("graph_numeric_batch_count_changed");
        add_appended_pages(node_ids, node_count, current.node_count());
        add_appended_pages(edge_ids, edge_count, current.edge_count());
        for (const auto& [address, value] : transition.numerical.signal.scores()) {
            (void)value;
            if (address >= current.node_count())
                throw std::runtime_error("graph_numeric_edit_address_invalid");
            node_ids.push_back(address / NativeGraphPageMap::records_per_page);
        }
        for (const auto& [address, value] : transition.numerical.signal.strengths()) {
            (void)value;
            if (address >= current.edge_count())
                throw std::runtime_error("graph_numeric_edit_address_invalid");
            edge_ids.push_back(address / NativeGraphPageMap::records_per_page);
        }
        node_count = current.node_count();
        edge_count = current.edge_count();
        graph_snapshot = current.snapshot_id();
        successor = transition.numerical.settled;
    }
    const auto& current = successor->require_validated_immutable();
    if (graph_snapshot != batch.graph_snapshot_id ||
        current.node_count() != node_count || current.edge_count() != edge_count)
        throw std::runtime_error("graph_numeric_batch_source_changed");
    finish_pages(node_ids);
    finish_pages(edge_ids);

    NativeGraphNumericPageResult result;
    result.numerical_successor = successor;
    result.node_page_updates.reserve(node_ids.size());
    result.edge_page_updates.reserve(edge_ids.size());
    for (const auto page_id : node_ids) {
        const auto old_count = live_in_page(old.node_count(), page_id);
        if (old_count != 0) {
            const auto physical = parent_pages.node_pages.offset(page_id);
            if (!physical)
                throw std::runtime_error("graph_numeric_parent_page_missing");
            require_old_node_page(node_file.read_node(*physical, page_id),
                                  old, old_count);
        } else if (parent_pages.node_pages.offset(page_id)) {
            throw std::runtime_error("graph_numeric_parent_page_changed");
        }
        NativeGraphNodePage page;
        page.page_id = page_id;
        page.valid_records = live_in_page(current.node_count(), page_id);
        const auto start = std::uint64_t(page_id) *
            NativeGraphPageMap::records_per_page;
        for (std::uint32_t at = 0; at < page.valid_records; ++at) {
            const auto address = static_cast<std::uint32_t>(start + at);
            page.records[at] = GraphNumericNodeRecord{
                current.direct(address), current.score(address),
                current.unresolved(address)};
        }
        result.node_page_updates.emplace_back(page_id, node_file.append(page));
    }
    for (const auto page_id : edge_ids) {
        const auto old_count = live_in_page(old.edge_count(), page_id);
        if (old_count != 0) {
            const auto physical = parent_pages.edge_pages.offset(page_id);
            if (!physical)
                throw std::runtime_error("graph_numeric_parent_page_missing");
            require_old_edge_page(edge_file.read_edge(*physical, page_id),
                                  old, old_count);
        } else if (parent_pages.edge_pages.offset(page_id)) {
            throw std::runtime_error("graph_numeric_parent_page_changed");
        }
        NativeGraphEdgePage page;
        page.page_id = page_id;
        page.valid_records = live_in_page(current.edge_count(), page_id);
        const auto start = std::uint64_t(page_id) *
            NativeGraphPageMap::records_per_page;
        for (std::uint32_t at = 0; at < page.valid_records; ++at) {
            const auto address = static_cast<std::uint32_t>(start + at);
            const auto edge = current.edge(address);
            if (edge.source >= current.node_count() ||
                edge.target >= current.node_count())
                throw std::runtime_error("graph_numeric_endpoint_changed");
            page.records[at] = GraphNumericEdgeRecord{
                edge, current.strength(address)};
        }
        result.edge_page_updates.emplace_back(page_id, edge_file.append(page));
    }
    node_file.sync();
    edge_file.sync();
    result.successor = NativeGraphNumericPageState{
        parent_pages.journal_generation, graph_snapshot,
        current.node_count(), current.edge_count(),
        parent_pages.node_pages.with_updates(result.node_page_updates),
        parent_pages.edge_pages.with_updates(result.edge_page_updates)};
    return result;
}

}  // namespace swegca::vrs
