#include "native_endpoint_manifest.hpp"

#include "digest.hpp"
#include "json.hpp"
#include "main_journal_append.hpp"
#include "memory_vrs_pair.hpp"
#include "native_endpoint_index.hpp"
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

constexpr std::string_view manifest_schema =
    "swegca-vrs2-endpoint-manifest-v1";

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
        throw std::runtime_error("endpoint_manifest_integer_invalid");
    return static_cast<std::int64_t>(value);
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:129-177
std::uint64_t nonnegative(const Json& value) {
    const auto number = value.integer();
    if (number < 0)
        throw std::runtime_error("endpoint_manifest_integer_invalid");
    return static_cast<std::uint64_t>(number);
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
        throw std::runtime_error("endpoint_manifest_source_changed");
    auto sequence = frame.first_sequence;
    source.visit_frame_rows(frame, [&](JournalRow&& row) {
        if (row.sequence != sequence || row.pair_id != pair ||
            parse_native_journal_entry(row.request_id, row.body,
                                       row.fingerprint).kind !=
                NativeJournalEntryKind::observation)
            throw std::runtime_error("endpoint_manifest_source_changed");
        ++sequence;
    });
    if (sequence != frame.last_sequence + 1)
        throw std::runtime_error("endpoint_manifest_source_changed");
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_dependency_index.py@7536139:40-49
Json segments_json(const std::vector<NativeEndpointSegment>& segments) {
    Json::Array result;
    result.reserve(segments.size());
    for (const auto& segment : segments) {
        Json::Array row;
        row.emplace_back(segment.path.filename().string());
        row.emplace_back(as_signed(segment.first_edge));
        row.emplace_back(as_signed(segment.edge_count));
        result.emplace_back(Json(std::move(row)));
    }
    return Json(std::move(result));
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_dependency_index.py@7536139:40-49
std::vector<NativeEndpointSegment> parse_segments(
    const Json& value,
    const std::filesystem::path& directory,
    std::string_view generation) {
    std::vector<NativeEndpointSegment> result;
    result.reserve(value.array().size());
    for (const auto& item : value.array()) {
        const auto& row = item.array();
        if (row.size() != 3)
            throw std::runtime_error("endpoint_manifest_segment_invalid");
        const auto& name = row[0].string();
        const std::filesystem::path relative(name);
        if (name.empty() || relative.has_parent_path() ||
            relative.filename() != relative ||
            name.find('/') != std::string::npos ||
            name.find('\\') != std::string::npos ||
            name.rfind("eps-", 0) != 0 || relative.extension() != ".vrs")
            throw std::runtime_error("endpoint_manifest_segment_invalid");
        result.push_back(NativeEndpointSegment{
            directory / relative, std::string(generation),
            nonnegative(row[1]), nonnegative(row[2])});
    }
    return result;
}

// Metadata validation is independent of physical file lifetime. Only the
// final active row needs its files after recovery.
// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_dependency_index.py@7536139:74-86
void require_partition_metadata(
    std::uint64_t edge_count,
    std::uint64_t base_edge_count,
    const std::vector<NativeEndpointSegment>& segments) {
    if (edge_count > std::numeric_limits<std::uint32_t>::max() ||
        base_edge_count > edge_count ||
        (base_edge_count != 0 && segments.empty()) ||
        (edge_count != 0 && segments.empty()))
        throw std::runtime_error("endpoint_manifest_partition_invalid");
    std::uint64_t next = 0;
    for (std::size_t at = 0; at < segments.size(); ++at) {
        const auto& segment = segments[at];
        if (segment.first_edge != next || segment.edge_count == 0 ||
            segment.edge_count > std::numeric_limits<std::uint32_t>::max() ||
            (at == 0 && base_edge_count != 0 &&
             segment.edge_count != base_edge_count) ||
            next > std::numeric_limits<std::uint32_t>::max() -
                       segment.edge_count)
            throw std::runtime_error("endpoint_manifest_partition_invalid");
        next += segment.edge_count;
    }
    if (next != edge_count)
        throw std::runtime_error("endpoint_manifest_partition_invalid");
    const auto first = base_edge_count == 0 ? std::size_t{0} : std::size_t{1};
    for (std::size_t at = first + 1; at < segments.size(); ++at) {
        const auto previous = segments[at - 1].edge_count;
        const auto current = segments[at].edge_count;
        if (current >= previous / 2 + previous % 2)
            throw std::runtime_error("endpoint_manifest_partition_invalid");
    }
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:129-177
std::string manifest_body(
    const NativeJournal& source,
    const JournalAppendResult& committed,
    const MainObservationBatchPlan& batch,
    const NativeEndpointManifestCursor& current,
    const NativeEndpointDependencyIndex& endpoints,
    const EventVrsInputView& numerical) {
    Json::Object body;
    body.emplace("schema", Json(std::string(manifest_schema)));
    body.emplace("source_generation", Json(source.generation()));
    body.emplace("source_file", Json(committed.frame.file_name));
    body.emplace("source_offset", Json(as_signed(committed.frame.byte_offset)));
    body.emplace("source_first", Json(committed.frame.first_sequence));
    body.emplace("source_last", Json(committed.frame.last_sequence));
    body.emplace("parent_graph", Json(current.graph_snapshot_id));
    body.emplace("graph", Json(numerical.snapshot_id()));
    body.emplace("parent_edges", Json(as_signed(current.edge_count)));
    body.emplace("edges", Json(as_signed(numerical.edge_count())));
    body.emplace("base_edges", Json(as_signed(endpoints.base_edge_count())));
    body.emplace("parent_pair", Json(batch.parent_pair_id));
    body.emplace("memory", Json(batch.memory_snapshot_id));
    body.emplace("segments", segments_json(endpoints.segments()));
    return Json(std::move(body)).canonical();
}

}  // namespace

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:223-257
NativeEndpointManifestCursor append_endpoint_manifest_row(
    NativeJournal& manifest_journal,
    const NativeJournal& source_journal,
    const JournalAppendResult& committed,
    const MainObservationBatchPlan& batch,
    const NativeEndpointManifestCursor& current,
    const NativeGraphNumericPageResult& numeric,
    std::string_view published_parent_pair) {
    std::uint64_t appended_edges = 0;
    for (const auto& transition : batch.graph_transitions) {
        if (transition.append.appended_edges.size() >
                std::numeric_limits<std::uint32_t>::max() - appended_edges)
            throw std::runtime_error("endpoint_manifest_source_changed");
        appended_edges += transition.append.appended_edges.size();
    }
    const auto& parent_graph = batch.graph_transitions.empty() ?
        batch.graph_snapshot_id :
        batch.graph_transitions.front().append.parent_snapshot_id;
    if (&manifest_journal == &source_journal ||
        manifest_journal.directory() == source_journal.directory() ||
        !manifest_journal.shares_write_owner(source_journal) ||
        manifest_journal.row_count() != current.manifest_rows ||
        current.journal_generation != source_journal.generation() ||
        current.endpoint_directory.empty() ||
        current.graph_snapshot_id != parent_graph ||
        current.pair_snapshot_id != batch.parent_pair_id ||
        current.edge_count > std::numeric_limits<std::uint32_t>::max() -
                                 appended_edges ||
        current.edge_count + appended_edges !=
            numeric.successor.edge_count ||
        numeric.successor.journal_generation != source_journal.generation() ||
        numeric.successor.graph_snapshot_id != batch.graph_snapshot_id ||
        !numeric.numerical_successor ||
        (committed.frame.first_sequence == current.last_source_sequence + 1 &&
         current.pair_snapshot_id != published_parent_pair) ||
        committed.frame.first_sequence <= current.last_source_sequence)
        throw std::runtime_error("endpoint_manifest_source_changed");
    require_committed_main_observation_frame(
        source_journal, committed, batch, published_parent_pair);
    const auto& numerical =
        numeric.numerical_successor->require_validated_immutable();
    const auto* endpoints = dynamic_cast<const NativeEndpointDependencyIndex*>(
        &numerical.dependencies());
    if (!endpoints || endpoints->directory() != current.endpoint_directory ||
        endpoints->journal_generation() != source_journal.generation() ||
        endpoints->base_edge_count() != current.base_edge_count ||
        numerical.snapshot_id() != numeric.successor.graph_snapshot_id ||
        numerical.edge_count() != numeric.successor.edge_count)
        throw std::runtime_error("endpoint_manifest_source_changed");
    endpoints->require_source(numerical);
    require_partition_metadata(numerical.edge_count(),
                               endpoints->base_edge_count(),
                               endpoints->segments());
    for (const auto& segment : endpoints->segments())
        NativeEndpointSegmentFile::validate_source(segment, numerical);
    const auto body = manifest_body(source_journal, committed, batch, current,
                                    *endpoints, numerical);
    PendingJournalRow row{
        "graph-endpoint-manifest:" +
            std::to_string(committed.frame.last_sequence),
        body, sha256_hex(body), batch.pair_snapshot_id};
    const auto appended = manifest_journal.append_addressed(
        std::span<const PendingJournalRow>(&row, 1));
    if (appended.sequences.size() != 1 ||
        appended.sequences.front() != as_signed(current.manifest_rows + 1))
        throw std::runtime_error("endpoint_manifest_append_changed");
    return NativeEndpointManifestCursor{
        current.endpoint_directory, source_journal.generation(),
        numerical.snapshot_id(), batch.pair_snapshot_id,
        numerical.edge_count(), endpoints->base_edge_count(),
        endpoints->segments(), current.manifest_rows + 1,
        committed.frame.last_sequence};
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:136-194
NativeEndpointManifestCursor recover_endpoint_manifest(
    const NativeJournal& manifest_journal,
    const NativeJournal& source_journal,
    std::filesystem::path endpoint_directory,
    std::string initial_graph_snapshot_id,
    std::string initial_pair_snapshot_id) {
    if (&manifest_journal == &source_journal ||
        manifest_journal.directory() == source_journal.directory() ||
        endpoint_directory.empty() || !sha256_id(initial_graph_snapshot_id) ||
        !sha256_id(initial_pair_snapshot_id))
        throw std::runtime_error("endpoint_manifest_initial_state_invalid");
    NativeEndpointManifestCursor cursor{
        std::move(endpoint_directory), source_journal.generation(),
        std::move(initial_graph_snapshot_id),
        std::move(initial_pair_snapshot_id), 0, 0, {}, 0, 0};
    manifest_journal.visit_rows(0, std::nullopt, [&](JournalRow&& row) {
        if (row.sequence != as_signed(cursor.manifest_rows + 1) ||
            row.fingerprint != sha256_hex(row.body))
            throw std::runtime_error("endpoint_manifest_journal_changed");
        const auto body = Json::parse(row.body);
        if (body.canonical() != row.body ||
            body.at("schema").string() != manifest_schema ||
            body.at("source_generation").string() !=
                source_journal.generation() ||
            body.at("parent_graph").string() != cursor.graph_snapshot_id ||
            nonnegative(body.at("parent_edges")) != cursor.edge_count ||
            body.at("parent_pair").string() != cursor.pair_snapshot_id ||
            !sha256_id(body.at("graph").string()) ||
            !sha256_id(body.at("memory").string()))
            throw std::runtime_error("endpoint_manifest_journal_changed");
        const auto frame = parse_source_frame(body);
        if (frame.first_sequence <= cursor.last_source_sequence ||
            row.request_id != "graph-endpoint-manifest:" +
                                  std::to_string(frame.last_sequence) ||
            row.pair_id != full_current_pair_snapshot_id(
                body.at("memory").string(), body.at("graph").string()))
            throw std::runtime_error("endpoint_manifest_source_changed");
        require_source_frame(source_journal, frame, row.pair_id);
        const auto edges = nonnegative(body.at("edges"));
        const auto base = nonnegative(body.at("base_edges"));
        auto segments = parse_segments(body.at("segments"),
                                       cursor.endpoint_directory,
                                       cursor.journal_generation);
        require_partition_metadata(edges, base, segments);
        if (base != cursor.base_edge_count || edges < cursor.edge_count)
            throw std::runtime_error("endpoint_manifest_source_changed");
        cursor.graph_snapshot_id = body.at("graph").string();
        cursor.pair_snapshot_id = row.pair_id;
        cursor.edge_count = edges;
        cursor.base_edge_count = base;
        cursor.segments = std::move(segments);
        cursor.last_source_sequence = frame.last_sequence;
        ++cursor.manifest_rows;
    });
    if (cursor.manifest_rows != manifest_journal.row_count())
        throw std::runtime_error("endpoint_manifest_journal_changed");
    for (const auto& segment : cursor.segments)
        NativeEndpointSegmentFile::validate_all(segment);
    return cursor;
}

}  // namespace swegca::vrs
