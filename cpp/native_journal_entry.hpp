#pragma once

#include "json.hpp"

#include <string_view>

namespace swegca::vrs {

enum class NativeJournalEntryKind {
    observation,
    alias,
    usage,
    consolidation,
};

struct NativeJournalEntry {
    NativeJournalEntryKind kind;
    Json value;
};

// Validate the complete author's native journal row taxonomy. Special rows
// remain source-bound VRS events; they are not converted to observations.
// SWEGCA: src/swegca_vrs2/store.py@c06092a:179-200
[[nodiscard]] NativeJournalEntry parse_native_journal_entry(
    std::string_view request_id, std::string_view body,
    std::string_view fingerprint);

}  // namespace swegca::vrs
