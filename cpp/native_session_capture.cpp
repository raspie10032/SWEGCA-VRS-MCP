#include "native_session_capture.hpp"

#include "digest.hpp"
#include "journal_files.hpp"
#include "memory_episode.hpp"
#include "owner_lock.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>
#else
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace swegca::vrs {
namespace {

constexpr std::string_view cursor_schema =
    "swegca-vrs2-session-capture-cursor-v2";
constexpr std::uint64_t max_line_bytes = 16 * 1024 * 1024;
constexpr std::uint64_t frame_budget = 1'048'576 - 128 * 1024;
constexpr std::uint64_t max_cursor_bytes = 8192;

struct CaptureCursor {
    std::uint64_t offset = 0;
    std::uint64_t line = 0;
    std::uint64_t captured = 0;
    std::uint64_t excluded = 0;
    std::vector<std::string> recent_user;
    std::string pair_snapshot_id;
};

struct TranscriptIdentity {
    std::string device;
    std::string file;
};

// SWEGCA: src/swegca_vrs2/session_capture.py@c06092a:189-207
std::string host_name(SessionHost host) {
    return host == SessionHost::codex ? "codex" : "claude";
}

// SWEGCA: src/swegca_vrs2/session_capture.py@c06092a:360-376
std::uint64_t nonnegative(const Json& value) {
    const auto number = value.integer();
    if (number < 0) throw std::runtime_error("session_cursor_invalid");
    return static_cast<std::uint64_t>(number);
}

// SWEGCA: src/swegca_vrs2/session_capture.py@c06092a:360-376
std::int64_t checked_integer(std::uint64_t value) {
    if (value > static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max()))
        throw std::runtime_error("session_cursor_overflow");
    return static_cast<std::int64_t>(value);
}

// SWEGCA: src/swegca_vrs2/session_capture.py@c06092a:360-376
void require_digest(std::string_view digest) {
    if (digest.size() != 64)
        throw std::runtime_error("session_cursor_invalid");
    for (const char digit : digest)
        if (!((digit >= '0' && digit <= '9') ||
              (digit >= 'a' && digit <= 'f')))
            throw std::runtime_error("session_cursor_invalid");
}

// SWEGCA: src/swegca_vrs2/session_capture.py@c06092a:145-150
CaptureCursor read_cursor(const std::filesystem::path& path,
                          SessionHost host, std::string_view session_key,
                          std::string_view path_digest,
                          const TranscriptIdentity& identity) {
    if (!std::filesystem::exists(path)) return {};
    if (!std::filesystem::is_regular_file(path) ||
        std::filesystem::file_size(path) > max_cursor_bytes)
        throw std::runtime_error("session_cursor_invalid");
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("session_cursor_invalid");
    const std::string bytes(std::istreambuf_iterator<char>{stream}, {});
    if (stream.bad()) throw std::runtime_error("session_cursor_invalid");
    const auto value = Json::parse(bytes);
    if (value.at("schema").string() != cursor_schema ||
        value.at("host").string() != host_name(host) ||
        value.at("session_key").string() != session_key ||
        value.at("path_digest").string() != path_digest)
        throw std::runtime_error("session_cursor_source_changed");
    if (value.at("source_device").string() != identity.device ||
        value.at("source_file").string() != identity.file)
        return {};
    CaptureCursor cursor;
    cursor.offset = nonnegative(value.at("offset"));
    cursor.line = nonnegative(value.at("line"));
    cursor.captured = nonnegative(value.at("captured"));
    cursor.excluded = nonnegative(value.at("excluded"));
    if (cursor.captured > cursor.line ||
        cursor.excluded != cursor.line - cursor.captured)
        throw std::runtime_error("session_cursor_accounting_changed");
    const auto& recent = value.at("recent_user").array();
    if (recent.size() > 4)
        throw std::runtime_error("session_cursor_invalid");
    for (const auto& identifier : recent) {
        const auto& text = identifier.string();
        if (!text.starts_with("memory:"))
            throw std::runtime_error("session_cursor_invalid");
        require_digest(std::string_view(text).substr(7));
        cursor.recent_user.push_back(text);
    }
    cursor.pair_snapshot_id = value.at("session_pair_id").string();
    if (!cursor.pair_snapshot_id.empty())
        require_digest(cursor.pair_snapshot_id);
    return cursor;
}

// SWEGCA: src/swegca_vrs2/session_capture.py@c06092a:123-143
void write_cursor(const std::filesystem::path& path,
                  SessionHost host, std::string_view session_key,
                  std::string_view path_digest,
                  const TranscriptIdentity& identity,
                  const CaptureCursor& cursor) {
    Json::Array recent;
    for (const auto& identifier : cursor.recent_user)
        recent.emplace_back(identifier);
    Json::Object body;
    body.emplace("schema", Json(std::string(cursor_schema)));
    body.emplace("host", Json(host_name(host)));
    body.emplace("session_key", Json(std::string(session_key)));
    body.emplace("path_digest", Json(std::string(path_digest)));
    body.emplace("source_device", Json(identity.device));
    body.emplace("source_file", Json(identity.file));
    body.emplace("offset", Json(checked_integer(cursor.offset)));
    body.emplace("line", Json(checked_integer(cursor.line)));
    body.emplace("captured", Json(checked_integer(cursor.captured)));
    body.emplace("excluded", Json(checked_integer(cursor.excluded)));
    body.emplace("recent_user", Json(std::move(recent)));
    body.emplace("session_pair_id", Json(cursor.pair_snapshot_id));
    const auto bytes = Json(std::move(body)).canonical();
    if (bytes.size() > max_cursor_bytes)
        throw std::runtime_error("session_cursor_invalid");
    write_atomic_file(path, std::as_bytes(std::span(bytes)));
}

// SWEGCA: src/swegca_vrs2/session_capture.py@c06092a:330-359
TranscriptIdentity require_transcript_source(const std::filesystem::path& path) {
    if (path.extension() != ".jsonl" ||
        !std::filesystem::is_regular_file(path))
        throw std::runtime_error("transcript_not_regular");
#if !defined(_WIN32)
    struct stat info {};
    if (::stat(path.c_str(), &info) != 0 ||
        info.st_uid != ::getuid())
        throw std::runtime_error("transcript_not_owned");
    return TranscriptIdentity{
        std::to_string(static_cast<std::uint64_t>(info.st_dev)),
        std::to_string(static_cast<std::uint64_t>(info.st_ino))};
#else
    const auto handle = ::CreateFileW(
        path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        throw std::runtime_error("transcript_file_identity_unavailable");
    BY_HANDLE_FILE_INFORMATION info{};
    const auto success = ::GetFileInformationByHandle(handle, &info);
    ::CloseHandle(handle);
    if (!success)
        throw std::runtime_error("transcript_file_identity_unavailable");
    const auto index =
        (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32) |
        info.nFileIndexLow;
    return TranscriptIdentity{
        std::to_string(info.dwVolumeSerialNumber), std::to_string(index)};
#endif
}

// SWEGCA: src/swegca_vrs2/session_capture.py@c06092a:388-434
std::optional<std::string> read_complete_line(std::istream& stream,
                                              bool& partial) {
    std::string line;
    line.reserve(4096);
    for (;;) {
        const auto next = stream.get();
        if (next == std::char_traits<char>::eof()) {
            if (stream.bad())
                throw std::runtime_error("session_transcript_read_failed");
            partial = !line.empty();
            return std::nullopt;
        }
        if (line.size() >= max_line_bytes)
            throw std::runtime_error("session_line_exceeds_memory_budget");
        line.push_back(static_cast<char>(next));
        if (next == '\n') return line;
    }
}

// SWEGCA: src/swegca_vrs2/session_capture.py@c06092a:388-434
void append_recent(CaptureCursor& cursor, std::string identifier) {
    cursor.recent_user.push_back(std::move(identifier));
    if (cursor.recent_user.size() > 4)
        cursor.recent_user.erase(cursor.recent_user.begin(),
                                 cursor.recent_user.end() - 4);
}

}  // namespace

// SWEGCA: src/swegca_vrs2/session_capture.py@c06092a:330-434
// SWEGCA: user@2026-09-22:54-62
SessionCaptureResult capture_session_transcript(
    const std::filesystem::path& state_root, SessionHost host,
    std::string_view session_id, const std::filesystem::path& transcript_path,
    SessionVrsIngest& session_vrs) {
    if (session_id.empty() || session_id.size() > 512)
        throw std::runtime_error("invalid_session");
    const auto session_key = sha256_hex(session_id);
    session_vrs.require_session(host, session_key);
    const auto path = std::filesystem::canonical(transcript_path);
    const auto identity = require_transcript_source(path);
    const auto path_text = path.string();
    const auto path_digest = sha256_hex(path_text);
    const auto root = std::filesystem::weakly_canonical(state_root);
    const auto cursor_path = root / "session-capture" / "cursors" /
        host_name(host) / session_key / (path_digest + ".json");
    OwnerLock lock(cursor_path.string() + ".lock");
    lock.acquire(std::chrono::milliseconds(0));
    auto cursor = read_cursor(cursor_path, host, session_key, path_digest,
                              identity);
    if (std::filesystem::file_size(path) < cursor.offset)
        cursor = {};
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("transcript_not_regular");
    stream.seekg(static_cast<std::streamoff>(cursor.offset));
    if (!stream) throw std::runtime_error("session_cursor_source_changed");
    auto safe = cursor;
    auto position = cursor.offset;
    std::vector<Json> pending;
    std::vector<std::string> pending_roles;
    std::uint64_t pending_bytes = 64;
    std::uint64_t processed = 0;
    const auto flush = [&](const CaptureCursor& checkpoint) {
        if (!pending.empty()) {
            const auto receipt = session_vrs.ingest_many(
                std::span<const Json>(pending));
            if (receipt.episode_ids.size() != pending.size() ||
                receipt.pair_snapshot_id.empty())
                throw std::runtime_error("session_vrs_batch_rejected");
            require_digest(receipt.pair_snapshot_id);
            for (std::size_t at = 0; at < pending.size(); ++at) {
                if (receipt.episode_ids[at] !=
                    episode_id_from_observation(pending[at]))
                    throw std::runtime_error("session_vrs_receipt_invalid");
                if (pending_roles[at] == "user")
                    append_recent(safe, receipt.episode_ids[at]);
            }
            processed += pending.size();
            safe.pair_snapshot_id = receipt.pair_snapshot_id;
            pending.clear();
            pending_roles.clear();
            pending_bytes = 64;
        }
        safe.offset = checkpoint.offset;
        safe.line = checkpoint.line;
        safe.captured = checkpoint.captured;
        safe.excluded = checkpoint.excluded;
        write_cursor(cursor_path, host, session_key, path_digest,
                     identity, safe);
    };
    bool partial = false;
    for (;;) {
        const auto raw = read_complete_line(stream, partial);
        if (!raw) break;
        const auto line_start = position;
        if (raw->size() > std::numeric_limits<std::uint64_t>::max() - position ||
            safe.line == std::numeric_limits<std::uint64_t>::max())
            throw std::runtime_error("session_cursor_overflow");
        position += raw->size();
        const auto next_line = safe.line + 1;
        const auto parts = visit_session_observations_for_line(
            host, session_key, path_text, *raw, line_start, next_line,
            [&](Json&& row) {
                const auto estimated = row.canonical().size() + 64;
                if (estimated > frame_budget)
                    throw std::runtime_error("session_observation_exceeds_frame");
                if (!pending.empty() &&
                    pending_bytes + estimated > frame_budget)
                    flush(safe);
                pending_bytes += estimated;
                pending_roles.push_back(
                    row.at("metadata").at("role").string());
                pending.push_back(std::move(row));
            });
        safe.offset = position;
        safe.line = next_line;
        if (parts) {
            if (safe.captured == std::numeric_limits<std::uint64_t>::max())
                throw std::runtime_error("session_cursor_overflow");
            ++safe.captured;
        } else {
            if (safe.excluded == std::numeric_limits<std::uint64_t>::max())
                throw std::runtime_error("session_cursor_overflow");
            ++safe.excluded;
        }
    }
    if (!pending.empty() || safe.offset != cursor.offset ||
        safe.line != cursor.line)
        flush(safe);
    return SessionCaptureResult{
        processed, safe.offset, safe.line, safe.captured, safe.excluded,
        partial, session_key};
}

}  // namespace swegca::vrs
