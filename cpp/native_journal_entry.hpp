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

// Validate the old 2.2 native journal row taxonomy. Alias, usage and
// consolidation remain provenance only in the SWEGCA rebuild; they are not
// converted to observations and execute no new-generation transition.
// SWEGCA: src/swegca_vrs2/store.py@c06092a:179-200
[[nodiscard]] NativeJournalEntry parse_native_journal_entry(
    std::string_view request_id, std::string_view body,
    std::string_view fingerprint);

}  // namespace swegca::vrs
