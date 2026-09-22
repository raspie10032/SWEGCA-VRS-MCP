#pragma once

#include "session_observation.hpp"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace swegca::vrs {

struct SessionIngestBatchReceipt {
    std::vector<std::string> episode_ids;
    std::string pair_snapshot_id;
};

// The concrete session Main coordinator implements this interface. No
// transcript row may bypass its HotIndex, Graph, journal, and pair commit.
// SWEGCA: src/swegca_vrs2/session_capture.py@c06092a:154-183
// SWEGCA: user@2026-09-22:54-62
class SessionVrsIngest {
public:
    virtual ~SessionVrsIngest() = default;
    virtual void require_session(SessionHost host,
                                 std::string_view session_key) const = 0;
    [[nodiscard]] virtual SessionIngestBatchReceipt ingest_many(
        std::span<const Json> observations) = 0;
};

struct SessionCaptureResult {
    std::uint64_t processed_observations;
    std::uint64_t offset;
    std::uint64_t line;
    std::uint64_t captured;
    std::uint64_t excluded;
    bool partial_line_waiting;
    std::string session_key;
};

// Read completed host JSONL lines into the session VRS only. The private
// cursor stores positions, counts, recent original addresses, and a pair ID;
// it is never a memory-recall source. Oversized lines remain at the previous
// safe cursor rather than being skipped or truncated.
// SWEGCA: src/swegca_vrs2/session_capture.py@c06092a:330-434
// SWEGCA: user@2026-09-22:54-62
[[nodiscard]] SessionCaptureResult capture_session_transcript(
    const std::filesystem::path& state_root, SessionHost host,
    std::string_view session_id,
    const std::filesystem::path& transcript_path,
    SessionVrsIngest& session_vrs);

}  // namespace swegca::vrs
