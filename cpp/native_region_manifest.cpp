#include "native_region_manifest.hpp"

#include "digest.hpp"
#include "json.hpp"
#include "memory_vrs_pair.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace swegca::vrs {
namespace {

constexpr std::string_view manifest_schema =
    "swegca-vrs2-region-manifest-v1";

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
        throw std::runtime_error("region_manifest_integer_invalid");
    return static_cast<std::int64_t>(value);
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:129-177
std::uint64_t nonnegative(const Json& value) {
    const auto number = value.integer();
    if (number < 0)
        throw std::runtime_error("region_manifest_integer_invalid");
    return static_cast<std::uint64_t>(number);
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_region_publication.py@0dc716a:37-53
bool explicit_false(const Json& value) {
    return std::holds_alternative<bool>(value.data) &&
        !std::get<bool>(value.data);
}

// SWEGCA: src/swegca_vrs2/store.py@c06092a:487-510
std::uint64_t required_pages(std::uint64_t node_count) {
    return node_count == 0 ? 0 :
        (node_count - 1) / NativeGraphPageMap::records_per_page + 1;
}

// SWEGCA: src/swegca_vrs2/store.py@c06092a:487-510
std::uint32_t expected_page_records(std::uint64_t node_count,
                                    std::uint32_t page) {
    const auto first = std::uint64_t{page} *
        NativeGraphPageMap::records_per_page;
    if (first >= node_count)
        throw std::runtime_error("region_manifest_page_invalid");
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(
        NativeGraphPageMap::records_per_page, node_count - first));
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:129-177
Json page_updates_json(
    std::span<const std::pair<std::uint32_t, std::uint64_t>> updates) {
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
        const auto& row = item.array();
        if (row.size() != 2)
            throw std::runtime_error("region_manifest_page_invalid");
        const auto page = nonnegative(row[0]);
        const auto offset = nonnegative(row[1]);
        if (page > NativeGraphPageMap::maximum_page_id || offset == 0 ||
            (!updates.empty() && page <= updates.back().first))
            throw std::runtime_error("region_manifest_page_invalid");
        updates.emplace_back(static_cast<std::uint32_t>(page), offset);
    }
    return updates;
}

// A changed Graph cannot inherit any old topology address: every topology is
// explicitly bound to the VRS snapshot from which it was built.
// SWEGCA: src/swegca_vrs2/store.py@c06092a:512-577
void require_update_shape(
    bool reset, std::uint64_t node_count,
    std::span<const std::pair<std::uint32_t, std::uint64_t>> updates) {
    const auto pages = required_pages(node_count);
    if (reset) {
        if (updates.size() != pages)
            throw std::runtime_error("region_manifest_reset_incomplete");
        for (std::uint64_t at = 0; at < pages; ++at)
            if (updates[at].first != at)
                throw std::runtime_error("region_manifest_reset_incomplete");
        return;
    }
    if (updates.empty())
        throw std::runtime_error("region_manifest_update_empty");
    for (const auto& [page, offset] : updates)
        if (page >= pages || offset == 0)
            throw std::runtime_error("region_manifest_page_invalid");
}

// SWEGCA: src/swegca_vrs2/store.py@c06092a:512-577
void require_page(
    const NativeRegionBindingPageFile& binding_file,
    const NativeRegionTopologyCatalog& catalog,
    std::string_view graph_snapshot_id,
    std::uint64_t node_count,
    std::uint32_t page_id,
    std::uint64_t offset) {
    const auto page = binding_file.read(offset, page_id);
    if (page.valid_records != expected_page_records(node_count, page_id))
        throw std::runtime_error("region_manifest_page_invalid");
    const auto first = std::uint64_t{page_id} *
        NativeGraphPageMap::records_per_page;
    for (std::uint32_t at = 0; at < page.valid_records; ++at) {
        const auto global = first + at;
        const auto& value = page.records[at];
        if (!value.present) continue;
        if (value.component > global || value.component >= node_count ||
            ((value.local == 0) != (value.component == global)) ||
            ((value.topology_offset != 0) !=
             (value.component == global)))
            throw std::runtime_error("region_manifest_binding_invalid");
        if (value.component != global) continue;
        const auto topology = catalog.open_topology(
            value.topology_offset, value.component);
        if (topology->vrs_snapshot_id() != graph_snapshot_id ||
            !topology->converged() || topology->term_count() == 0 ||
            topology->term(0) != global)
            throw std::runtime_error("region_manifest_topology_invalid");
    }
}

// SWEGCA: src/swegca_vrs2/store.py@c06092a:512-577
void require_updates(
    const NativeRegionBindingPageFile& binding_file,
    const NativeRegionTopologyCatalog& catalog,
    std::string_view graph_snapshot_id,
    std::uint64_t node_count,
    std::span<const std::pair<std::uint32_t, std::uint64_t>> updates) {
    for (const auto& [page, offset] : updates)
        require_page(binding_file, catalog, graph_snapshot_id,
                     node_count, page, offset);
}

// SWEGCA: src/swegca_vrs2/store.py@c06092a:512-577
void require_complete_state(
    const NativeRegionBindingState& state,
    const NativeRegionBindingPageFile& binding_file,
    const NativeRegionTopologyCatalog& catalog) {
    const auto pages = required_pages(state.node_count);
    std::uint64_t visited = 0;
    state.pages.visit([&](std::uint32_t page, std::uint64_t offset) {
        if (page != visited || page >= pages)
            throw std::runtime_error("region_manifest_page_map_invalid");
        require_page(binding_file, catalog, state.graph_snapshot_id,
                     state.node_count, page, offset);
        ++visited;
    });
    if (visited != pages)
        throw std::runtime_error("region_manifest_page_map_invalid");
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:129-177
std::string manifest_body(
    const NativeRegionManifestCursor& current,
    std::string_view source_generation,
    std::string_view graph_snapshot_id,
    std::uint64_t node_count,
    std::string_view memory_snapshot_id,
    std::int64_t published_source_rows,
    bool reset,
    std::span<const std::pair<std::uint32_t, std::uint64_t>> updates) {
    Json::Object body;
    body.emplace("schema", Json(std::string(manifest_schema)));
    body.emplace("source_generation", Json(std::string(source_generation)));
    body.emplace("parent_graph", Json(current.bindings.graph_snapshot_id));
    body.emplace("parent_nodes", Json(as_signed(current.bindings.node_count)));
    body.emplace("parent_memory", Json(current.memory_snapshot_id));
    body.emplace("parent_pair", Json(current.pair_snapshot_id));
    body.emplace("parent_source_rows", Json(current.published_source_rows));
    body.emplace("graph", Json(std::string(graph_snapshot_id)));
    body.emplace("nodes", Json(as_signed(node_count)));
    body.emplace("memory", Json(std::string(memory_snapshot_id)));
    body.emplace("source_rows", Json(published_source_rows));
    body.emplace("reset", Json(static_cast<std::int64_t>(reset ? 1 : 0)));
    body.emplace("page_updates", page_updates_json(updates));
    body.emplace("grants_authority", Json(false));
    return Json(std::move(body)).canonical();
}

}  // namespace

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_region_publication.py@0dc716a:37-53
// SWEGCA: src/swegca_vrs2/store.py@c06092a:512-577
NativeRegionManifestCursor append_region_manifest_row(
    NativeJournal& manifest_journal,
    const NativeJournal& source_journal,
    const NativeRegionManifestCursor& current,
    std::string graph_snapshot_id,
    std::uint64_t node_count,
    std::string memory_snapshot_id,
    std::string pair_snapshot_id,
    std::int64_t published_source_rows,
    std::span<const std::pair<std::uint32_t, std::uint64_t>> page_updates,
    const NativeRegionBindingPageFile& binding_file,
    const NativeRegionTopologyCatalog& catalog)
// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_region_publication.py@0dc716a:37-53
// SWEGCA: src/swegca_vrs2/store.py@c06092a:512-577
{
    const auto first = current.manifest_rows == 0;
    const auto reset = first ||
        graph_snapshot_id != current.bindings.graph_snapshot_id;
    const auto source_head = source_journal.head();
    if (&manifest_journal == &source_journal ||
        manifest_journal.directory() == source_journal.directory() ||
        !manifest_journal.shares_write_owner(source_journal) ||
        manifest_journal.row_count() != current.manifest_rows ||
        binding_file.journal_generation() != source_journal.generation() ||
        catalog.journal_generation() != source_journal.generation() ||
        !sha256_id(graph_snapshot_id) || !sha256_id(memory_snapshot_id) ||
        !sha256_id(pair_snapshot_id) ||
        pair_snapshot_id != full_current_pair_snapshot_id(
            memory_snapshot_id, graph_snapshot_id) ||
        node_count > std::uint64_t{0x100000000ULL} ||
        published_source_rows <= 0 ||
        static_cast<std::uint64_t>(published_source_rows) !=
            source_journal.row_count() ||
        !source_head || source_head->first != published_source_rows ||
        source_head->second != pair_snapshot_id)
        throw std::runtime_error("region_manifest_source_changed");
    if (first) {
        if (!current.bindings.journal_generation.empty() ||
            !current.bindings.graph_snapshot_id.empty() ||
            current.bindings.node_count != 0 ||
            current.bindings.pages.page_count() != 0 ||
            !current.memory_snapshot_id.empty() ||
            !current.pair_snapshot_id.empty() ||
            current.published_source_rows != 0)
            throw std::runtime_error("region_manifest_initial_state_invalid");
    } else if (current.bindings.journal_generation !=
                   source_journal.generation() ||
               current.bindings.pages.page_count() !=
                   required_pages(current.bindings.node_count) ||
               node_count < current.bindings.node_count ||
               published_source_rows < current.published_source_rows ||
               (!reset &&
                (node_count != current.bindings.node_count ||
                 memory_snapshot_id != current.memory_snapshot_id ||
                 pair_snapshot_id != current.pair_snapshot_id ||
                 published_source_rows != current.published_source_rows)) ||
               (reset &&
                published_source_rows <= current.published_source_rows)) {
        throw std::runtime_error("region_manifest_source_changed");
    }
    require_update_shape(reset, node_count, page_updates);
    require_updates(binding_file, catalog, graph_snapshot_id,
                    node_count, page_updates);
    NativeGraphPageMap empty;
    const auto pages = (reset ? empty : current.bindings.pages)
        .with_updates(page_updates);
    if (pages.page_count() != required_pages(node_count))
        throw std::runtime_error("region_manifest_page_map_invalid");
    const auto body = manifest_body(
        current, source_journal.generation(), graph_snapshot_id,
        node_count, memory_snapshot_id, published_source_rows,
        reset, page_updates);
    PendingJournalRow row{
        "graph-region-manifest:" +
            std::to_string(current.manifest_rows + 1),
        body, sha256_hex(body), pair_snapshot_id};
    const auto appended = manifest_journal.append_addressed(
        std::span<const PendingJournalRow>(&row, 1));
    if (appended.sequences.size() != 1 ||
        appended.sequences.front() != as_signed(current.manifest_rows + 1))
        throw std::runtime_error("region_manifest_append_changed");
    return NativeRegionManifestCursor{
        NativeRegionBindingState{
            source_journal.generation(), std::move(graph_snapshot_id),
            node_count, pages},
        std::move(memory_snapshot_id), std::move(pair_snapshot_id),
        published_source_rows, current.manifest_rows + 1};
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:136-194
// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_region_publication.py@0dc716a:37-53
NativeRegionManifestCursor recover_region_manifest(
    const NativeJournal& manifest_journal,
    const NativeJournal& source_journal,
    const NativeRegionBindingPageFile& binding_file,
    const NativeRegionTopologyCatalog& catalog) {
    if (&manifest_journal == &source_journal ||
        manifest_journal.directory() == source_journal.directory() ||
        binding_file.journal_generation() != source_journal.generation() ||
        catalog.journal_generation() != source_journal.generation())
        throw std::runtime_error("region_manifest_initial_state_invalid");
    NativeRegionManifestCursor cursor;
    manifest_journal.visit_rows(0, std::nullopt, [&](JournalRow&& row) {
        if (row.sequence != as_signed(cursor.manifest_rows + 1) ||
            row.fingerprint != sha256_hex(row.body) ||
            row.request_id != "graph-region-manifest:" +
                                  std::to_string(cursor.manifest_rows + 1))
            throw std::runtime_error("region_manifest_journal_changed");
        const auto body = Json::parse(row.body);
        if (body.canonical() != row.body ||
            body.at("schema").string() != manifest_schema ||
            body.at("source_generation").string() !=
                source_journal.generation() ||
            body.at("parent_graph").string() !=
                cursor.bindings.graph_snapshot_id ||
            nonnegative(body.at("parent_nodes")) !=
                cursor.bindings.node_count ||
            body.at("parent_memory").string() != cursor.memory_snapshot_id ||
            body.at("parent_pair").string() != cursor.pair_snapshot_id ||
            body.at("parent_source_rows").integer() !=
                cursor.published_source_rows ||
            nonnegative(body.at("reset")) > 1 ||
            !explicit_false(body.at("grants_authority")))
            throw std::runtime_error("region_manifest_journal_changed");
        const auto graph = body.at("graph").string();
        const auto memory = body.at("memory").string();
        const auto nodes = nonnegative(body.at("nodes"));
        const auto source_rows = body.at("source_rows").integer();
        const auto reset = nonnegative(body.at("reset")) == 1;
        const auto expected_reset = cursor.manifest_rows == 0 ||
            graph != cursor.bindings.graph_snapshot_id;
        if (!sha256_id(graph) || !sha256_id(memory) ||
            !sha256_id(row.pair_id) ||
            row.pair_id != full_current_pair_snapshot_id(memory, graph) ||
            nodes > std::uint64_t{0x100000000ULL} || source_rows <= 0 ||
            source_rows < cursor.published_source_rows ||
            reset != expected_reset ||
            (cursor.manifest_rows != 0 &&
             nodes < cursor.bindings.node_count) ||
            (!reset &&
             (nodes != cursor.bindings.node_count ||
              memory != cursor.memory_snapshot_id ||
              row.pair_id != cursor.pair_snapshot_id ||
              source_rows != cursor.published_source_rows)) ||
            (reset && cursor.manifest_rows != 0 &&
             source_rows <= cursor.published_source_rows))
            throw std::runtime_error("region_manifest_source_changed");
        const auto updates = parse_page_updates(body.at("page_updates"));
        require_update_shape(reset, nodes, updates);
        NativeGraphPageMap empty;
        auto pages = (reset ? empty : cursor.bindings.pages)
            .with_updates(updates);
        if (pages.page_count() != required_pages(nodes))
            throw std::runtime_error("region_manifest_page_map_invalid");
        cursor.bindings = NativeRegionBindingState{
            source_journal.generation(), graph, nodes, std::move(pages)};
        cursor.memory_snapshot_id = memory;
        cursor.pair_snapshot_id = row.pair_id;
        cursor.published_source_rows = source_rows;
        ++cursor.manifest_rows;
    });
    if (cursor.manifest_rows != manifest_journal.row_count())
        throw std::runtime_error("region_manifest_journal_changed");
    if (cursor.manifest_rows == 0) return cursor;
    if (static_cast<std::uint64_t>(cursor.published_source_rows) >
            source_journal.row_count())
        throw std::runtime_error("region_manifest_source_changed");
    const auto source_pair = source_journal.pair(cursor.published_source_rows);
    if (!source_pair || *source_pair != cursor.pair_snapshot_id)
        throw std::runtime_error("region_manifest_source_changed");
    require_complete_state(cursor.bindings, binding_file, catalog);
    return cursor;
}

}  // namespace swegca::vrs
