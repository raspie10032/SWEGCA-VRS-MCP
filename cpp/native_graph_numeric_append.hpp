#pragma once

#include "main_observation_batch.hpp"
#include "native_graph_page_file.hpp"
#include "native_journal.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace swegca::vrs {

// The page maps identify immutable bytes belonging to one Graph generation.
// The original journal remains the authority for experience and batch order.
// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_delta.py@7536139:117-180
struct NativeGraphNumericPageState {
    std::string journal_generation;
    std::string graph_snapshot_id;
    std::uint64_t node_count = 0;
    std::uint64_t edge_count = 0;
    NativeGraphPageMap node_pages;
    NativeGraphPageMap edge_pages;
};

struct NativeGraphNumericPageResult {
    NativeGraphNumericPageState successor;
    std::shared_ptr<const ValidatedEventVrsInputs> numerical_successor;
    std::vector<std::pair<std::uint32_t, std::uint64_t>> node_page_updates;
    std::vector<std::pair<std::uint32_t, std::uint64_t>> edge_page_updates;
};

// A committed author's Graph.append_many batch supplies the only values that
// may enter new numerical pages. This function copies only pages containing
// appended or edited addresses, checks any replaced old page against the
// pinned parent input, and syncs both page files before returning unpublished
// map updates. Main must persist and certify those updates with the pair.
// SWEGCA: src/swegca_vrs2/store.py@c06092a:427-512
// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_delta.py@7536139:117-180
[[nodiscard]] NativeGraphNumericPageResult append_committed_graph_numeric_pages(
    const NativeJournal& journal, const JournalAppendResult& committed,
    const MainObservationBatchPlan& batch,
    std::shared_ptr<const ValidatedEventVrsInputs> parent,
    const NativeGraphNumericPageState& parent_pages,
    std::string_view published_parent_pair,
    NativeGraphPageFile& node_file, NativeGraphPageFile& edge_file);

}  // namespace swegca::vrs
