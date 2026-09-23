#pragma once

#include "native_session_lifecycle.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace swegca::vrs {

struct LinkedSessionShard {
    SessionHost host;
    std::string session_key;
    SessionShardSeal seal;
};

struct SessionLinkReceipt {
    std::vector<std::string> shard_ids;
    std::uint64_t added;
    std::uint64_t original_count;
    std::uint64_t cue_total;
};

// Main ownership contains native VRS locations, never transcript content.
// The visitor streams complete session-link events in original attach order.
// SWEGCA: src/swegca_vrs2/linked_shards.py@c06092a:70-94
void visit_linked_sessions(
    const std::filesystem::path& state_root,
    const std::function<void(const LinkedSessionShard&)>& visit);

// The whole ended-session shard batch is published atomically. Identical
// retries are no-ops; ID, path or generation reassignment is rejected.
// Lineage: weak analogy — the author rewrites a JSON registry under a lock; here one journal link event per ended session, reassignment rejected.
// SWEGCA: src/swegca_vrs2/linked_shards.py@c06092a:105-140
// SWEGCA: user@2026-09-22:68
[[nodiscard]] SessionLinkReceipt attach_ended_session(
    const std::filesystem::path& state_root, SessionHost host,
    std::string_view session_id);

}  // namespace swegca::vrs
