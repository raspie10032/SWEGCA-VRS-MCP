#pragma once

#include "native_graph_numeric_append.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace swegca::vrs {

// One derived map row follows one already committed sequence of original
// Graph row transitions.
// The original journal and its pair remain authoritative; this cursor holds
// only physical page addresses and the last source row it covers.
// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:136-194
struct NativeGraphMapCursor {
    NativeGraphNumericPageState pages;
    std::uint64_t map_rows = 0;
    std::int64_t last_source_sequence = 0;
    std::string pair_snapshot_id;
};

// Append the newly synced page map under the same Main owner lock. A durable
// error requires recovery before retry, so a Graph batch cannot receive two
// different derived map rows.
// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:223-257
[[nodiscard]] NativeGraphMapCursor append_graph_numeric_map_row(
    NativeJournal& map_journal, const NativeJournal& source_journal,
    const JournalAppendResult& committed,
    const MainObservationBatchPlan& batch,
    const NativeGraphMapCursor& current,
    const NativeGraphNumericPageResult& numeric,
    std::string_view published_parent_pair,
    const NativeGraphPageFile& node_file,
    const NativeGraphPageFile& edge_file);

// Rebuild the bounded in-memory page address map from the derived journal.
// Every row is rebound to one exact original observation frame and each
// changed physical page is checked before it enters the recovered map.
// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:136-194
[[nodiscard]] NativeGraphMapCursor recover_graph_numeric_map(
    const NativeJournal& map_journal,
    const NativeJournal& source_journal,
    NativeGraphNumericPageState empty_state,
    std::string initial_pair_snapshot_id,
    const NativeGraphPageFile& node_file,
    const NativeGraphPageFile& edge_file);

}  // namespace swegca::vrs
