#pragma once

#include "exact_journal_directory.hpp"
#include "hot_index_projection_log.hpp"
#include "main_observation_batch.hpp"
#include "native_cue_directory.hpp"
#include "native_journal.hpp"
#include "native_operation_directory.hpp"

#include <cstdint>
#include <string_view>

namespace swegca::vrs {

struct MainMemoryProjectionAppendCount {
    std::uint64_t journaled_observations;
    std::uint64_t new_originals;
    std::uint64_t duplicate_originals;
};

// Apply one already durable observation frame to unpublished or row-limited
// derived directories. Each original, raw posting, and request certificate
// stays bound to its exact journal row. Main publishes no reader here.
// SWEGCA: src/swegca_vrs2/store.py@c06092a:1416-1452
// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:496-609
[[nodiscard]] MainMemoryProjectionAppendCount
append_main_memory_projections(
    const NativeJournal& journal, const JournalAppendResult& committed,
    const MainObservationBatchPlan& plan, std::string_view published_parent_pair,
    ExactJournalDirectory& originals, HotIndexProjectionLog& headers,
    NativeCueDirectory& cues, NativeCueDirectory& propositions,
    NativeCueDirectory& successors, NativeCueDirectory& sources,
    NativeOperationDirectory& operations);

}  // namespace swegca::vrs
