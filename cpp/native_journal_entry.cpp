#include "native_journal_entry.hpp"

#include "digest.hpp"
#include "observation.hpp"

#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

namespace swegca::vrs {

// SWEGCA: src/swegca_vrs2/store.py@c06092a:179-200
NativeJournalEntry parse_native_journal_entry(
    std::string_view request_id, std::string_view body,
    std::string_view fingerprint) {
    auto entry = Json::parse(body);
    if (const auto* object = std::get_if<Json::Object>(&entry.data)) {
        const auto found = object->find("kind");
        if (found != object->end()) {
            if (const auto* kind = std::get_if<std::string>(&found->second.data)) {
                NativeJournalEntryKind classified;
                std::string_view prefix;
                if (*kind == "alias") {
                    classified = NativeJournalEntryKind::alias;
                    prefix = "alias:";
                } else if (*kind == "usage") {
                    classified = NativeJournalEntryKind::usage;
                    prefix = "usage:";
                } else if (*kind == "consolidation") {
                    classified = NativeJournalEntryKind::consolidation;
                    prefix = "consolidation:";
                } else {
                    classified = NativeJournalEntryKind::observation;
                }
                if (!prefix.empty()) {
                    if (!request_id.starts_with(prefix) ||
                        sha256_hex(entry.canonical()) != fingerprint)
                        throw std::runtime_error(
                            "stored_observation_integrity_failed");
                    return NativeJournalEntry{classified, std::move(entry)};
                }
            }
        }
    }
    auto row = observation(entry);
    if (row.at("request_id").string() != request_id ||
        sha256_hex(row.canonical()) != fingerprint)
        throw std::runtime_error("stored_observation_integrity_failed");
    return NativeJournalEntry{NativeJournalEntryKind::observation,
                              std::move(row)};
}

}  // namespace swegca::vrs
