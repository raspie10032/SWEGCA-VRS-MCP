#pragma once

#include "native_session_capture.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace swegca::vrs {

struct SessionShardSeal {
    std::string identifier;
    std::filesystem::path directory;
    std::string journal_generation;
    std::string pair_snapshot_id;
    std::uint64_t journal_rows;
    std::uint64_t original_count;
    std::uint64_t cue_total;
};

// The concrete session Main owner closes writers and returns certificates for
// every complete native VRS shard. A repeated call after a crash must return
// the same seals. Finalization verifies their journal heads before any ended
// marker can authorize Main attachment.
// SWEGCA: src/swegca_vrs2/session_capture.py@c06092a:461-494
class SessionVrsFinalizer : public SessionVrsIngest {
public:
    [[nodiscard]] virtual std::vector<SessionShardSeal> seal_after_end() = 0;
};

// Called only by the host's real SessionEnd dispatcher. Interrupt calls the
// capture-only path and cannot create this marker.
// SWEGCA: src/swegca_vrs2/session_capture.py@c06092a:436-453
// SWEGCA: user@2026-09-22:72-79
void mark_session_end_intent(
    const std::filesystem::path& state_root, SessionHost host,
    std::string_view session_id,
    const std::filesystem::path& transcript_path);

// Runs off the prompt path after SessionEnd. It waits for a complete stable
// final transcript tail, then seals and verifies the session VRS generations.
// The returned seals are source-bound candidates for the separate atomic
// Main ownership registry; the transcript itself is never the recall store.
// SWEGCA: src/swegca_vrs2/conversation_finalize.py@c06092a:14-47
// SWEGCA: user@2026-09-22:63-79
[[nodiscard]] std::vector<SessionShardSeal> finalize_session_end(
    const std::filesystem::path& state_root, SessionHost host,
    std::string_view session_id,
    const std::filesystem::path& transcript_path,
    SessionVrsFinalizer& session_vrs);

// Main reopens the durable real-SessionEnd certificate and verifies the
// sealed native journals before accepting ownership. No transcript is read.
// SWEGCA: src/swegca_vrs2/session_capture.py@c06092a:461-494
// SWEGCA: src/swegca_vrs2/linked_shards.py@c06092a:39-66
[[nodiscard]] std::vector<SessionShardSeal> read_verified_ended_session(
    const std::filesystem::path& state_root, SessionHost host,
    std::string_view session_id);

}  // namespace swegca::vrs
