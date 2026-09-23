#pragma once

#include "native_journal_entry.hpp"
#include "native_linked_sessions.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace swegca::vrs {

// Main owns the transaction. Staged rows are fed in their original journal
// order; commit must be durable and idempotent by request ID and fingerprint.
// A retried source group cannot create another original experience.
// Lineage: withdrawn flow — the author's batch ingest commits in order, idempotent by request ID; here the Main sink the withdrawn session replay (ORDER 30b73e7:69,76-78) feeds.
// SWEGCA: src/swegca_vrs2/store.py@c06092a:1389-1460
// SWEGCA: user@2026-09-23:112-114
class MainVrsIntegrationSink {
public:
    virtual ~MainVrsIntegrationSink() = default;
    virtual void begin_group(const LinkedSessionShard& source,
                             std::string_view source_pair_id) = 0;
    virtual void stage(std::string_view request_id,
                       std::int64_t source_sequence,
                       NativeJournalEntry&& entry) = 0;
    [[nodiscard]] virtual std::string commit_group() = 0;
    virtual void abort_group() noexcept = 0;
};

struct SessionIntegrationReceipt {
    std::string shard_id;
    std::uint64_t applied_rows;
    std::uint64_t final_sequence;
    std::string last_main_pair_id;
};

// Runs only for a linked, ended session and never on the host input path.
// The source journal is the only content source; the transcript is not read.
// A durable cursor advances only after Main's group commit. Crash retry uses
// the author's request-ID idempotence instead of copying experience bodies.
// Lineage: withdrawn flow — replays a linked, ended session into Main by batch ingest, never the transcript; first ordered by ORDER 30b73e7:69,76-78, then replaced by the user on 2026-09-23 with one kept block and its connection points.
// SWEGCA: src/swegca_vrs2/store.py@c06092a:1389-1460
// SWEGCA: user@2026-09-23:112-114
[[nodiscard]] SessionIntegrationReceipt integrate_linked_session_shard(
    const std::filesystem::path& state_root,
    const LinkedSessionShard& linked,
    MainVrsIntegrationSink& main);

}  // namespace swegca::vrs
