#pragma once

#include "main_observation_batch.hpp"
#include "native_endpoint_segment_file.hpp"
#include "native_graph_numeric_append.hpp"
#include "native_journal.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace swegca::vrs {

// Durable cursor for the active endpoint segment partition. Each manifest row
// is derived from one already committed original observation frame; it does
// not replace that frame or confer authority on its own.
// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:136-194
struct NativeEndpointManifestCursor {
    std::filesystem::path endpoint_directory;
    std::string journal_generation;
    std::string graph_snapshot_id;
    std::string pair_snapshot_id;
    std::uint64_t edge_count = 0;
    std::uint64_t base_edge_count = 0;
    std::vector<NativeEndpointSegment> segments;
    std::uint64_t manifest_rows = 0;
    std::int64_t last_source_sequence = 0;
};

// Record one fully synced active segment partition under the same Main owner
// lock as the original and numerical-map journals.
// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:223-257
[[nodiscard]] NativeEndpointManifestCursor append_endpoint_manifest_row(
    NativeJournal& manifest_journal,
    const NativeJournal& source_journal,
    const JournalAppendResult& committed,
    const MainObservationBatchPlan& batch,
    const NativeEndpointManifestCursor& current,
    const NativeGraphNumericPageResult& numeric,
    std::string_view published_parent_pair);

// Replay the manifest chain, then validate only the final active immutable
// files. Historical files may already have been reclaimed after old readers
// released their pinned generations.
// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:136-194
[[nodiscard]] NativeEndpointManifestCursor recover_endpoint_manifest(
    const NativeJournal& manifest_journal,
    const NativeJournal& source_journal,
    std::filesystem::path endpoint_directory,
    std::string initial_graph_snapshot_id,
    std::string initial_pair_snapshot_id);

}  // namespace swegca::vrs
