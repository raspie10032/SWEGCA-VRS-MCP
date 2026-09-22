#pragma once

#include "json.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace swegca::vrs {

enum class SessionHost { codex, claude };

struct HostVisibleRecord {
    std::string role;
    std::string content;
    std::string record_type;
};

// Source transcripts are ingress only. Private reasoning payloads are never
// admitted as public experience; visible summaries and tool results are.
// SWEGCA: src/swegca_vrs2/session_capture.py@c06092a:42-111
[[nodiscard]] std::optional<HostVisibleRecord> host_visible_record(
    SessionHost host, const Json& row);

// Every emitted row passes the author's observation validator. One source
// line may produce multiple ordered original experiences. The emitter runs
// synchronously, so a large line does not accumulate all parts in RAM.
// Metadata retains raw-line and normalized-content byte spans for Replay.
// SWEGCA: src/swegca_vrs2/session_capture.py@c06092a:316-328
// SWEGCA: user@2026-09-22:54-62
[[nodiscard]] std::uint64_t visit_session_observations_for_line(
    SessionHost host, std::string_view session_key,
    std::string_view transcript_path, std::string_view raw_line,
    std::uint64_t line_start_byte, std::uint64_t line_number,
    const std::function<void(Json&&)>& emit);

}  // namespace swegca::vrs
