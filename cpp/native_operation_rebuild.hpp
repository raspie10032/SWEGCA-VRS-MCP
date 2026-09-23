#pragma once

#include "native_journal.hpp"
#include "native_operation_directory.hpp"

#include <cstdint>

namespace swegca::vrs {

struct NativeOperationRebuildCount {
    std::uint64_t journal_rows;
    std::uint64_t observations;
    std::uint64_t provenance_rows;
};

// The journal is authoritative. Only author observation ingress occupies the
// request operation namespace. Recognized 2.2 non-author typed rows remain
// validated provenance in the journal and do not execute a Graph transition
// or reserve an observation request ID in the rebuilt generation.
// SWEGCA: src/swegca_vrs2/store.py@7536139:378-399
[[nodiscard]] NativeOperationRebuildCount rebuild_native_operation_directory(
    const NativeJournal& journal, NativeOperationDirectory& operations);

}  // namespace swegca::vrs
