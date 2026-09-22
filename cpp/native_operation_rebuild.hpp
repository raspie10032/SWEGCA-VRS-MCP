#pragma once

#include "native_journal.hpp"
#include "native_operation_directory.hpp"

#include <cstdint>

namespace swegca::vrs {

struct NativeOperationRebuildCount {
    std::uint64_t journal_rows;
    std::uint64_t observations;
    std::uint64_t graph_events;
};

// The journal is authoritative. Every operation kind retains the author's
// first request-ID fingerprint, optional original ID, and historical pair ID.
// A distinct digest prefix has one ordered worker; no all-operations map is
// required in RAM. Publication follows Main's complete pair verification.
// SWEGCA: src/swegca_vrs2/store.py@c06092a:895-933
// SWEGCA: src/swegca_vrs2/store.py@7536139:378-399
[[nodiscard]] NativeOperationRebuildCount rebuild_native_operation_directory(
    const NativeJournal& journal, NativeOperationDirectory& operations);

}  // namespace swegca::vrs
