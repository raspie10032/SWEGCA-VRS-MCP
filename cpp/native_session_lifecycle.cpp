#include "native_session_lifecycle.hpp"

#include "digest.hpp"
#include "journal_files.hpp"
#include "native_journal.hpp"
#include "owner_lock.hpp"

#include <chrono>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace swegca::vrs {
namespace {

constexpr std::string_view intent_schema =
    "swegca-vrs2-session-end-intent-v1";
constexpr std::string_view ended_schema =
    "swegca-vrs2-session-ended-v1";

// SWEGCA: src/swegca_vrs2/session_capture.py@c06092a:189-210
std::string host_name(SessionHost host) {
    return host == SessionHost::codex ? "codex" : "claude";
}

// SWEGCA: src/swegca_vrs2/session_capture.py@c06092a:194-210
std::string session_key(std::string_view session_id) {
    if (session_id.empty() || session_id.size() > 512)
        throw std::runtime_error("invalid_session");
    return sha256_hex(session_id);
}

// SWEGCA: src/swegca_vrs2/session_capture.py@c06092a:123-150
Json read_marker(const std::filesystem::path& path) {
    if (!std::filesystem::is_regular_file(path) ||
        std::filesystem::file_size(path) > 65536)
        throw std::runtime_error("session_end_marker_invalid");
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("session_end_marker_invalid");
    const std::string bytes(std::istreambuf_iterator<char>{stream}, {});
    if (stream.bad()) throw std::runtime_error("session_end_marker_invalid");
    return Json::parse(bytes);
}

// SWEGCA: src/swegca_vrs2/session_capture.py@c06092a:436-453
Json intent_body(SessionHost host, std::string_view key,
                 std::string_view path_digest) {
    Json::Object body;
    body.emplace("schema", Json(std::string(intent_schema)));
    body.emplace("event", Json(std::string("SessionEnd")));
    body.emplace("host", Json(host_name(host)));
    body.emplace("session_key", Json(std::string(key)));
    body.emplace("transcript_path_digest", Json(std::string(path_digest)));
    return Json(std::move(body));
}

// SWEGCA: src/swegca_vrs2/session_capture.py@c06092a:436-453
std::filesystem::path intent_path(const std::filesystem::path& root,
                                  SessionHost host, std::string_view key) {
    return root / "session-capture" / "ending" / host_name(host) /
        (std::string(key) + ".json");
}

// SWEGCA: src/swegca_vrs2/session_capture.py@c06092a:436-453
void require_digest(std::string_view digest) {
    if (digest.size() != 64)
        throw std::runtime_error("session_seal_digest_invalid");
    for (char digit : digest)
        if (!((digit >= '0' && digit <= '9') ||
              (digit >= 'a' && digit <= 'f')))
            throw std::runtime_error("session_seal_digest_invalid");
}

// SWEGCA: src/swegca_vrs2/linked_shards.py@c06092a:39-66
std::filesystem::path verified_inside(
    const std::filesystem::path& directory,
    const std::filesystem::path& session_root) {
    const auto resolved = std::filesystem::canonical(directory);
    const auto relative = resolved.lexically_relative(session_root);
    if (relative.empty() || relative.is_absolute())
        throw std::runtime_error("session_shard_path_invalid");
    for (const auto& component : relative)
        if (component == "..")
            throw std::runtime_error("session_shard_path_invalid");
    return resolved;
}

// SWEGCA: src/swegca_vrs2/linked_shards.py@c06092a:39-66
std::int64_t checked_count(std::uint64_t value) {
    if (value > static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max()))
        throw std::runtime_error("session_shard_count_invalid");
    return static_cast<std::int64_t>(value);
}

// SWEGCA: src/swegca_vrs2/session_capture.py@c06092a:461-494
// SWEGCA: src/swegca_vrs2/linked_shards.py@c06092a:39-66
Json verified_seals(const std::filesystem::path& root,
                    const std::filesystem::path& session_root,
                    const std::vector<SessionShardSeal>& seals) {
    if (seals.empty())
        throw std::runtime_error("session_shard_batch_empty");
    std::set<std::string> identifiers;
    std::set<std::filesystem::path> directories;
    Json::Array rows;
    rows.reserve(seals.size());
    for (const auto& seal : seals) {
        const auto directory = verified_inside(
            seal.directory, session_root);
        if (!seal.identifier.starts_with("session-") ||
            seal.identifier.size() > 126 ||
            !identifiers.insert(seal.identifier).second ||
            !directories.insert(directory).second ||
            !is_native_store(directory))
            throw std::runtime_error("session_shard_invalid");
        require_digest(seal.pair_snapshot_id);
        OwnerLock lock(directory / "owner.lock");
        lock.acquire(std::chrono::milliseconds(0));
        NativeJournal journal(directory, false, false);
        if (journal.generation() != seal.journal_generation ||
            journal.row_count() != seal.journal_rows ||
            seal.original_count > seal.journal_rows)
            throw std::runtime_error("session_shard_generation_changed");
        const auto head = journal.head();
        if ((head && head->second != seal.pair_snapshot_id) ||
            (!head && seal.journal_rows != 0))
            throw std::runtime_error("session_shard_pair_changed");
        Json::Object row;
        row.emplace("id", Json(seal.identifier));
        row.emplace("path", Json(
            std::filesystem::relative(directory, root).generic_string()));
        row.emplace("journal_generation", Json(seal.journal_generation));
        row.emplace("pair_snapshot_id", Json(seal.pair_snapshot_id));
        row.emplace("journal_rows", Json(checked_count(seal.journal_rows)));
        row.emplace("records", Json(checked_count(seal.original_count)));
        row.emplace("cue_total", Json(checked_count(seal.cue_total)));
        rows.emplace_back(Json(std::move(row)));
    }
    return Json(std::move(rows));
}

// SWEGCA: src/swegca_vrs2/session_capture.py@c06092a:436-494
std::filesystem::path ended_path(const std::filesystem::path& root,
                                 SessionHost host, std::string_view key) {
    return root / "session-capture" / "ended" / host_name(host) /
        (std::string(key) + ".json");
}

}  // namespace

// SWEGCA: src/swegca_vrs2/session_capture.py@c06092a:436-453
// SWEGCA: user@2026-09-22:72-79
void mark_session_end_intent(
    const std::filesystem::path& state_root, SessionHost host,
    std::string_view session_id,
    const std::filesystem::path& transcript_path) {
    const auto root = std::filesystem::weakly_canonical(state_root);
    const auto key = session_key(session_id);
    const auto path = std::filesystem::canonical(transcript_path);
    const auto body = intent_body(host, key, sha256_hex(path.string()));
    const auto marker = intent_path(root, host, key);
    OwnerLock lock(marker.string() + ".lock");
    lock.acquire(std::chrono::milliseconds(0));
    if (std::filesystem::exists(marker)) {
        if (read_marker(marker).canonical() != body.canonical())
            throw std::runtime_error("session_end_intent_reassigned");
        return;
    }
    const auto bytes = body.canonical();
    write_atomic_file(marker, std::as_bytes(std::span(bytes)));
}

// SWEGCA: src/swegca_vrs2/conversation_finalize.py@c06092a:14-47
// SWEGCA: src/swegca_vrs2/session_capture.py@c06092a:461-494
// SWEGCA: user@2026-09-22:63-79
std::vector<SessionShardSeal> finalize_session_end(
    const std::filesystem::path& state_root, SessionHost host,
    std::string_view session_id,
    const std::filesystem::path& transcript_path,
    SessionVrsFinalizer& session_vrs) {
    const auto root = std::filesystem::weakly_canonical(state_root);
    const auto key = session_key(session_id);
    const auto transcript = std::filesystem::canonical(transcript_path);
    const auto expected = intent_body(
        host, key, sha256_hex(transcript.string())).canonical();
    if (read_marker(intent_path(root, host, key)).canonical() != expected)
        throw std::runtime_error("session_end_intent_missing");
    OwnerLock watcher(
        root / "session-capture" / "watchers" / host_name(host) /
        (key + ".lock"));
    watcher.acquire(std::chrono::milliseconds(120000));
    session_vrs.require_session(host, key);
    std::uint32_t stable = 0;
    bool complete = false;
    for (std::uint32_t attempt = 0; attempt < 50; ++attempt) {
        const auto capture = capture_session_transcript(
            root, host, session_id, transcript, session_vrs);
        const auto size = std::filesystem::file_size(transcript);
        if (!capture.partial_line_waiting && capture.offset == size) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (std::filesystem::file_size(transcript) == size) {
                if (++stable >= 10) {
                    complete = true;
                    break;
                }
            } else {
                stable = 0;
            }
        } else {
            stable = 0;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    if (!complete)
        throw std::runtime_error("session_transcript_not_stable");
    auto seals = session_vrs.seal_after_end();
    const auto session_root = std::filesystem::canonical(
        root / "session-vrs" / host_name(host) / key);
    const auto rows = verified_seals(root, session_root, seals);
    Json::Object body;
    body.emplace("schema", Json(std::string(ended_schema)));
    body.emplace("event", Json(std::string("SessionEnd")));
    body.emplace("host", Json(host_name(host)));
    body.emplace("session_key", Json(key));
    body.emplace("transcript_path_digest",
                 Json(sha256_hex(transcript.string())));
    body.emplace("shards", rows);
    const auto marker = ended_path(root, host, key);
    const auto bytes = Json(std::move(body)).canonical();
    if (std::filesystem::exists(marker)) {
        if (read_marker(marker).canonical() != bytes)
            throw std::runtime_error("session_ended_marker_reassigned");
    } else {
        write_atomic_file(marker, std::as_bytes(std::span(bytes)));
    }
    return seals;
}

// SWEGCA: src/swegca_vrs2/session_capture.py@c06092a:461-494
// SWEGCA: src/swegca_vrs2/linked_shards.py@c06092a:39-66
std::vector<SessionShardSeal> read_verified_ended_session(
    const std::filesystem::path& state_root, SessionHost host,
    std::string_view session_id) {
    const auto root = std::filesystem::weakly_canonical(state_root);
    const auto key = session_key(session_id);
    const auto marker = read_marker(ended_path(root, host, key));
    if (marker.object().size() != 6 ||
        marker.at("schema").string() != ended_schema ||
        marker.at("event").string() != "SessionEnd" ||
        marker.at("host").string() != host_name(host) ||
        marker.at("session_key").string() != key)
        throw std::runtime_error("session_ended_marker_invalid");
    const auto& transcript_digest =
        marker.at("transcript_path_digest").string();
    require_digest(transcript_digest);
    if (read_marker(intent_path(root, host, key)).canonical() !=
        intent_body(host, key, transcript_digest).canonical())
        throw std::runtime_error("session_end_intent_missing");
    const auto session_root = std::filesystem::canonical(
        root / "session-vrs" / host_name(host) / key);
    const auto& rows = marker.at("shards").array();
    std::vector<SessionShardSeal> seals;
    seals.reserve(rows.size());
    for (const auto& raw : rows) {
        if (raw.object().size() != 7)
            throw std::runtime_error("session_ended_shard_invalid");
        const auto& relative = raw.at("path").string();
        const auto path = std::filesystem::path(relative);
        if (relative.empty() || path.is_absolute() ||
            path.lexically_normal().generic_string() != relative)
            throw std::runtime_error("session_ended_shard_path_invalid");
        const auto count = raw.at("journal_rows").integer();
        const auto records = raw.at("records").integer();
        const auto cues = raw.at("cue_total").integer();
        if (count < 0 || records < 0 || cues < 0)
            throw std::runtime_error("session_ended_shard_count_invalid");
        seals.push_back(SessionShardSeal{
            raw.at("id").string(), root / path,
            raw.at("journal_generation").string(),
            raw.at("pair_snapshot_id").string(),
            static_cast<std::uint64_t>(count),
            static_cast<std::uint64_t>(records),
            static_cast<std::uint64_t>(cues)});
    }
    if (verified_seals(root, session_root, seals).canonical() !=
        marker.at("shards").canonical())
        throw std::runtime_error("session_ended_shard_changed");
    return seals;
}

}  // namespace swegca::vrs
