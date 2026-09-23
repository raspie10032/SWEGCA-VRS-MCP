#include "native_graph_map_journal.hpp"

#include "digest.hpp"
#include "json.hpp"
#include "main_journal_append.hpp"
#include "memory_vrs_pair.hpp"
#include "native_journal_entry.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace swegca::vrs {
namespace {

constexpr std::string_view map_schema = "swegca-vrs2-graph-numeric-map-v1";

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:44-45
bool sha256_id(std::string_view identifier) {
    return identifier.size() == 64 &&
        std::all_of(identifier.begin(), identifier.end(), [](char digit) {
            return (digit >= '0' && digit <= '9') ||
                   (digit >= 'a' && digit <= 'f');
        });
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:129-177
std::int64_t as_signed(std::uint64_t value) {
    if (value > static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max()))
        throw std::runtime_error("graph_numeric_map_integer_invalid");
    return static_cast<std::int64_t>(value);
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:129-177
std::uint64_t nonnegative(const Json& value) {
    const auto number = value.integer();
    if (number < 0) throw std::runtime_error("graph_numeric_map_integer_invalid");
    return static_cast<std::uint64_t>(number);
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:129-177
Json page_updates_json(
    const std::vector<std::pair<std::uint32_t, std::uint64_t>>& updates) {
    Json::Array rows;
    rows.reserve(updates.size());
    for (const auto& [page, offset] : updates) {
        Json::Array row;
        row.emplace_back(static_cast<std::int64_t>(page));
        row.emplace_back(as_signed(offset));
        rows.emplace_back(Json(std::move(row)));
    }
    return Json(std::move(rows));
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:129-177
std::vector<std::pair<std::uint32_t, std::uint64_t>> parse_page_updates(
    const Json& value) {
    std::vector<std::pair<std::uint32_t, std::uint64_t>> updates;
    updates.reserve(value.array().size());
    for (const auto& item : value.array()) {
        const auto& pair = item.array();
        if (pair.size() != 2)
            throw std::runtime_error("graph_numeric_map_page_invalid");
        const auto page = nonnegative(pair[0]);
        const auto offset = nonnegative(pair[1]);
        if (page > NativeGraphPageMap::maximum_page_id || offset == 0 ||
            (!updates.empty() && page <= updates.back().first))
            throw std::runtime_error("graph_numeric_map_page_invalid");
        updates.emplace_back(static_cast<std::uint32_t>(page), offset);
    }
    return updates;
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:136-177
JournalFrameAddress parse_source_frame(const Json& body) {
    return JournalFrameAddress{
        body.at("source_generation").string(),
        body.at("source_file").string(),
        nonnegative(body.at("source_offset")),
        body.at("source_first").integer(),
        body.at("source_last").integer()};
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:136-177
void require_source_frame(const NativeJournal& source,
                          const JournalFrameAddress& frame,
                          std::string_view pair) {
    if (frame.generation != source.generation() ||
        frame.first_sequence <= 0 ||
        frame.last_sequence < frame.first_sequence ||
        static_cast<std::uint64_t>(frame.last_sequence) > source.row_count())
        throw std::runtime_error("graph_numeric_map_source_changed");
    std::int64_t sequence = frame.first_sequence;
    source.visit_frame_rows(frame, [&](JournalRow&& row) {
        if (row.sequence != sequence || row.pair_id != pair ||
            parse_native_journal_entry(row.request_id, row.body,
                                       row.fingerprint).kind !=
                NativeJournalEntryKind::observation)
            throw std::runtime_error("graph_numeric_map_source_changed");
        ++sequence;
    });
    if (sequence != frame.last_sequence + 1)
        throw std::runtime_error("graph_numeric_map_source_changed");
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_delta.py@7536139:141-163
std::uint64_t page_count(std::uint64_t records) {
    return records == 0 ? 0 :
        (records - 1) / NativeGraphPageMap::records_per_page + 1;
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_delta.py@7536139:141-163
std::uint32_t expected_page_records(std::uint64_t records,
                                    std::uint32_t page) {
    const auto first = std::uint64_t(page) *
        NativeGraphPageMap::records_per_page;
    if (first >= records)
        throw std::runtime_error("graph_numeric_map_page_invalid");
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(
        NativeGraphPageMap::records_per_page, records - first));
}

// A new address cannot refer to an old partial page without an updated page
// pointer; the count check alone would miss that omission.
// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_delta.py@7536139:138-163
void require_appended_pages(
    std::uint64_t old_count, std::uint64_t next_count,
    const std::vector<std::pair<std::uint32_t, std::uint64_t>>& updates) {
    if (next_count < old_count || next_count > std::uint64_t{0x100000000ULL})
        throw std::runtime_error("graph_numeric_map_count_invalid");
    if (next_count == old_count) return;
    const auto first = old_count / NativeGraphPageMap::records_per_page;
    const auto last = (next_count - 1) / NativeGraphPageMap::records_per_page;
    for (auto page = first; page <= last; ++page) {
        const auto found = std::lower_bound(updates.begin(), updates.end(), page,
            [](const auto& item, std::uint64_t probe) {
                return item.first < probe;
            });
        if (found == updates.end() || found->first != page)
            throw std::runtime_error("graph_numeric_map_append_page_missing");
    }
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:34-76
void require_physical_pages(
    const std::vector<std::pair<std::uint32_t, std::uint64_t>>& nodes,
    const std::vector<std::pair<std::uint32_t, std::uint64_t>>& edges,
    std::uint64_t node_count, std::uint64_t edge_count,
    const NativeGraphPageFile& node_file,
    const NativeGraphPageFile& edge_file) {
    for (const auto& [page, offset] : nodes)
        if (node_file.read_node(offset, page).valid_records !=
            expected_page_records(node_count, page))
            throw std::runtime_error("graph_numeric_map_page_invalid");
    for (const auto& [page, offset] : edges)
        if (edge_file.read_edge(offset, page).valid_records !=
            expected_page_records(edge_count, page))
            throw std::runtime_error("graph_numeric_map_page_invalid");
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:129-177
std::string map_body(const NativeJournal& source,
                     const JournalAppendResult& committed,
                     const MainObservationBatchPlan& batch,
                     const NativeGraphMapCursor& current,
                     const NativeGraphNumericPageResult& numeric) {
    Json::Object body;
    body.emplace("schema", Json(std::string(map_schema)));
    body.emplace("source_generation", Json(source.generation()));
    body.emplace("source_file", Json(committed.frame.file_name));
    body.emplace("source_offset", Json(as_signed(committed.frame.byte_offset)));
    body.emplace("source_first", Json(committed.frame.first_sequence));
    body.emplace("source_last", Json(committed.frame.last_sequence));
    body.emplace("parent_graph", Json(current.pages.graph_snapshot_id));
    body.emplace("graph", Json(numeric.successor.graph_snapshot_id));
    body.emplace("parent_nodes", Json(as_signed(current.pages.node_count)));
    body.emplace("parent_edges", Json(as_signed(current.pages.edge_count)));
    body.emplace("nodes", Json(as_signed(numeric.successor.node_count)));
    body.emplace("edges", Json(as_signed(numeric.successor.edge_count)));
    body.emplace("parent_pair", Json(batch.parent_pair_id));
    body.emplace("memory", Json(batch.memory_snapshot_id));
    body.emplace("node_pages", page_updates_json(numeric.node_page_updates));
    body.emplace("edge_pages", page_updates_json(numeric.edge_page_updates));
    return Json(std::move(body)).canonical();
}

}  // namespace

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:223-257
NativeGraphMapCursor append_graph_numeric_map_row(
    NativeJournal& map_journal, const NativeJournal& source_journal,
    const JournalAppendResult& committed,
    const MainObservationBatchPlan& batch,
    const NativeGraphMapCursor& current,
    const NativeGraphNumericPageResult& numeric,
    std::string_view published_parent_pair,
    const NativeGraphPageFile& node_file,
    const NativeGraphPageFile& edge_file) {
    std::uint64_t appended_nodes = 0;
    std::uint64_t appended_edges = 0;
    for (const auto& transition : batch.graph_transitions) {
        if (transition.append.new_nodes.size() >
                std::numeric_limits<std::uint64_t>::max() - appended_nodes ||
            transition.append.appended_edges.size() >
                std::numeric_limits<std::uint64_t>::max() - appended_edges)
            throw std::runtime_error("graph_numeric_map_source_changed");
        appended_nodes += transition.append.new_nodes.size();
        appended_edges += transition.append.appended_edges.size();
    }
    const auto& parent_graph = batch.graph_transitions.empty() ?
        batch.graph_snapshot_id :
        batch.graph_transitions.front().append.parent_snapshot_id;
    if (&map_journal == &source_journal ||
        map_journal.directory() == source_journal.directory() ||
        !map_journal.shares_write_owner(source_journal) ||
        map_journal.row_count() != current.map_rows ||
        current.pages.journal_generation != source_journal.generation() ||
        node_file.journal_generation() != source_journal.generation() ||
        edge_file.journal_generation() != source_journal.generation() ||
        current.pages.graph_snapshot_id != parent_graph ||
        numeric.successor.graph_snapshot_id != batch.graph_snapshot_id ||
        numeric.successor.journal_generation != source_journal.generation() ||
        numeric.successor.node_count !=
            current.pages.node_count + appended_nodes ||
        numeric.successor.edge_count !=
            current.pages.edge_count + appended_edges ||
        !numeric.numerical_successor ||
        numeric.numerical_successor->require_validated_immutable().snapshot_id() !=
            numeric.successor.graph_snapshot_id ||
        numeric.numerical_successor->require_validated_immutable().node_count() !=
            numeric.successor.node_count ||
        numeric.numerical_successor->require_validated_immutable().edge_count() !=
            numeric.successor.edge_count ||
        (committed.frame.first_sequence == current.last_source_sequence + 1 &&
         current.pair_snapshot_id != published_parent_pair) ||
        committed.frame.first_sequence <= current.last_source_sequence)
        throw std::runtime_error("graph_numeric_map_source_changed");
    require_committed_main_observation_frame(
        source_journal, committed, batch, published_parent_pair);
    require_appended_pages(current.pages.node_count,
                           numeric.successor.node_count,
                           numeric.node_page_updates);
    require_appended_pages(current.pages.edge_count,
                           numeric.successor.edge_count,
                           numeric.edge_page_updates);
    const auto node_map = current.pages.node_pages.with_updates(
        numeric.node_page_updates);
    const auto edge_map = current.pages.edge_pages.with_updates(
        numeric.edge_page_updates);
    if (node_map.page_count() != page_count(numeric.successor.node_count) ||
        edge_map.page_count() != page_count(numeric.successor.edge_count))
        throw std::runtime_error("graph_numeric_map_page_invalid");
    require_physical_pages(numeric.node_page_updates,
                           numeric.edge_page_updates,
                           numeric.successor.node_count,
                           numeric.successor.edge_count,
                           node_file, edge_file);
    const auto body = map_body(source_journal, committed, batch, current,
                               numeric);
    PendingJournalRow row{
        "graph-numeric-map:" + std::to_string(committed.frame.last_sequence),
        body, sha256_hex(body), batch.pair_snapshot_id};
    const auto appended = map_journal.append_addressed(
        std::span<const PendingJournalRow>(&row, 1));
    if (appended.sequences.size() != 1 ||
        appended.sequences.front() != as_signed(current.map_rows + 1))
        throw std::runtime_error("graph_numeric_map_append_changed");
    return NativeGraphMapCursor{
        NativeGraphNumericPageState{
            source_journal.generation(), numeric.successor.graph_snapshot_id,
            numeric.successor.node_count, numeric.successor.edge_count,
            node_map, edge_map},
        current.map_rows + 1, committed.frame.last_sequence,
        batch.pair_snapshot_id};
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:136-194
NativeGraphMapCursor recover_graph_numeric_map(
    const NativeJournal& map_journal,
    const NativeJournal& source_journal,
    NativeGraphNumericPageState empty_state,
    std::string initial_pair_snapshot_id,
    const NativeGraphPageFile& node_file,
    const NativeGraphPageFile& edge_file) {
    if (&map_journal == &source_journal ||
        map_journal.directory() == source_journal.directory() ||
        empty_state.journal_generation != source_journal.generation() ||
        node_file.journal_generation() != source_journal.generation() ||
        edge_file.journal_generation() != source_journal.generation() ||
        empty_state.node_count != 0 || empty_state.edge_count != 0 ||
        empty_state.node_pages.page_count() != 0 ||
        empty_state.edge_pages.page_count() != 0 ||
        !sha256_id(empty_state.graph_snapshot_id) ||
        !sha256_id(initial_pair_snapshot_id))
        throw std::runtime_error("graph_numeric_map_initial_state_invalid");
    NativeGraphMapCursor cursor{
        std::move(empty_state), 0, 0, std::move(initial_pair_snapshot_id)};
    map_journal.visit_rows(0, std::nullopt, [&](JournalRow&& row) {
        if (row.sequence != as_signed(cursor.map_rows + 1) ||
            row.fingerprint != sha256_hex(row.body))
            throw std::runtime_error("graph_numeric_map_journal_changed");
        const auto body = Json::parse(row.body);
        if (body.canonical() != row.body ||
            body.at("schema").string() != map_schema ||
            body.at("source_generation").string() !=
                source_journal.generation() ||
            body.at("parent_graph").string() !=
                cursor.pages.graph_snapshot_id ||
            nonnegative(body.at("parent_nodes")) != cursor.pages.node_count ||
            nonnegative(body.at("parent_edges")) != cursor.pages.edge_count ||
            !sha256_id(body.at("graph").string()) ||
            !sha256_id(body.at("memory").string()) ||
            !sha256_id(body.at("parent_pair").string()))
            throw std::runtime_error("graph_numeric_map_journal_changed");
        const auto frame = parse_source_frame(body);
        if (frame.first_sequence <= cursor.last_source_sequence ||
            (frame.first_sequence == cursor.last_source_sequence + 1 &&
             body.at("parent_pair").string() != cursor.pair_snapshot_id) ||
            row.request_id !=
                "graph-numeric-map:" + std::to_string(frame.last_sequence) ||
            row.pair_id != full_current_pair_snapshot_id(
                body.at("memory").string(), body.at("graph").string()))
            throw std::runtime_error("graph_numeric_map_source_changed");
        require_source_frame(source_journal, frame, row.pair_id);
        const auto nodes = nonnegative(body.at("nodes"));
        const auto edges = nonnegative(body.at("edges"));
        if (nodes <= cursor.pages.node_count ||
            nodes > std::uint64_t{0x100000000ULL} ||
            edges > std::numeric_limits<std::uint32_t>::max())
            throw std::runtime_error("graph_numeric_map_count_invalid");
        const auto node_updates = parse_page_updates(body.at("node_pages"));
        const auto edge_updates = parse_page_updates(body.at("edge_pages"));
        require_appended_pages(cursor.pages.node_count, nodes, node_updates);
        require_appended_pages(cursor.pages.edge_count, edges, edge_updates);
        const auto node_map = cursor.pages.node_pages.with_updates(node_updates);
        const auto edge_map = cursor.pages.edge_pages.with_updates(edge_updates);
        if (node_map.page_count() != page_count(nodes) ||
            edge_map.page_count() != page_count(edges))
            throw std::runtime_error("graph_numeric_map_page_invalid");
        require_physical_pages(node_updates, edge_updates, nodes, edges,
                               node_file, edge_file);
        cursor.pages = NativeGraphNumericPageState{
            source_journal.generation(), body.at("graph").string(),
            nodes, edges, node_map, edge_map};
        cursor.last_source_sequence = frame.last_sequence;
        cursor.pair_snapshot_id = row.pair_id;
        ++cursor.map_rows;
    });
    if (cursor.map_rows != map_journal.row_count())
        throw std::runtime_error("graph_numeric_map_journal_changed");
    return cursor;
}

}  // namespace swegca::vrs
