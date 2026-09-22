#include "native_linked_sessions.hpp"

#include "digest.hpp"
#include "journal_files.hpp"
#include "native_journal.hpp"
#include "owner_lock.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace swegca::vrs {
namespace {

constexpr std::string_view schema = "swegca-vrs2-native-session-link-v1";

// SWEGCA: src/swegca_vrs2/session_capture.py@c06092a:194-210
std::string host_name(SessionHost host) {
    return host == SessionHost::codex ? "codex" : "claude";
}

// SWEGCA: src/swegca_vrs2/linked_shards.py@c06092a:54-66
void require_digest(std::string_view value) {
    if (value.size() != 64)
        throw std::runtime_error("linked_shard_digest_invalid");
    for (const char digit : value)
        if (!((digit >= '0' && digit <= '9') ||
              (digit >= 'a' && digit <= 'f')))
            throw std::runtime_error("linked_shard_digest_invalid");
}

// SWEGCA: src/swegca_vrs2/linked_shards.py@c06092a:39-66
std::filesystem::path checked_path(const std::filesystem::path& root,
    SessionHost host, std::string_view key, const std::filesystem::path& path) {
    const auto parent = std::filesystem::canonical(
        root / "session-vrs" / host_name(host) / std::string(key));
    const auto directory = std::filesystem::canonical(path);
    const auto relative = directory.lexically_relative(parent);
    if (relative.empty() || relative.is_absolute())
        throw std::runtime_error("linked_shard_path_invalid");
    for (const auto& component : relative)
        if (component == "..")
            throw std::runtime_error("linked_shard_path_invalid");
    if (!is_native_store(directory))
        throw std::runtime_error("linked_shard_path_invalid");
    return directory;
}

// SWEGCA: src/swegca_vrs2/linked_shards.py@c06092a:97-138
std::int64_t count(std::uint64_t value) {
    if (value > static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max()))
        throw std::runtime_error("linked_shard_count_invalid");
    return static_cast<std::int64_t>(value);
}

// SWEGCA: src/swegca_vrs2/linked_shards.py@c06092a:39-66
Json serial_row(const std::filesystem::path& root,
                const LinkedSessionShard& linked) {
    require_digest(linked.session_key);
    require_digest(linked.seal.pair_snapshot_id);
    const auto& seal = linked.seal;
    if (!seal.identifier.starts_with("session-") ||
        seal.identifier.size() > 126 || seal.journal_generation.empty() ||
        seal.original_count > seal.journal_rows)
        throw std::runtime_error("linked_shard_invalid");
    const auto directory = checked_path(
        root, linked.host, linked.session_key, seal.directory);
    Json::Object row;
    row.emplace("host", Json(host_name(linked.host)));
    row.emplace("session_key", Json(linked.session_key));
    row.emplace("id", Json(seal.identifier));
    row.emplace("path", Json(
        std::filesystem::relative(directory, root).generic_string()));
    row.emplace("journal_generation", Json(seal.journal_generation));
    row.emplace("pair_snapshot_id", Json(seal.pair_snapshot_id));
    row.emplace("journal_rows", Json(count(seal.journal_rows)));
    row.emplace("records", Json(count(seal.original_count)));
    row.emplace("cue_total", Json(count(seal.cue_total)));
    return Json(std::move(row));
}

// SWEGCA: src/swegca_vrs2/linked_shards.py@c06092a:39-94
LinkedSessionShard parse_row(const std::filesystem::path& root,
                             const Json& raw) {
    if (raw.object().size() != 9)
        throw std::runtime_error("linked_shard_invalid");
    const auto& name = raw.at("host").string();
    if (name != "codex" && name != "claude")
        throw std::runtime_error("linked_shard_host_invalid");
    const auto& relative = raw.at("path").string();
    const auto path = std::filesystem::path(relative);
    if (relative.empty() || path.is_absolute() ||
        path.lexically_normal().generic_string() != relative)
        throw std::runtime_error("linked_shard_path_invalid");
    const auto rows = raw.at("journal_rows").integer();
    const auto records = raw.at("records").integer();
    const auto cues = raw.at("cue_total").integer();
    if (rows < 0 || records < 0 || cues < 0 || records > rows)
        throw std::runtime_error("linked_shard_count_invalid");
    LinkedSessionShard linked{
        name == "codex" ? SessionHost::codex : SessionHost::claude,
        raw.at("session_key").string(),
        SessionShardSeal{raw.at("id").string(), root / path,
            raw.at("journal_generation").string(),
            raw.at("pair_snapshot_id").string(),
            static_cast<std::uint64_t>(rows),
            static_cast<std::uint64_t>(records),
            static_cast<std::uint64_t>(cues)}};
    if (serial_row(root, linked).canonical() != raw.canonical())
        throw std::runtime_error("linked_shard_changed");
    return linked;
}

// SWEGCA: src/swegca_vrs2/linked_shards.py@c06092a:97-138
Json link_event(const std::filesystem::path& root, SessionHost host,
                std::string_view key,
                const std::vector<SessionShardSeal>& seals) {
    Json::Array rows;
    rows.reserve(seals.size());
    for (const auto& seal : seals)
        rows.push_back(serial_row(root, LinkedSessionShard{
            host, std::string(key), seal}));
    Json::Object body;
    body.emplace("schema", Json(std::string(schema)));
    body.emplace("host", Json(host_name(host)));
    body.emplace("session_key", Json(std::string(key)));
    body.emplace("shards", Json(std::move(rows)));
    return Json(std::move(body));
}

// SWEGCA: src/swegca_vrs2/linked_shards.py@c06092a:70-94
std::vector<LinkedSessionShard> parse_link_event(
    const std::filesystem::path& root, const JournalRow& stored) {
    const auto body = Json::parse(stored.body);
    if (body.object().size() != 4 || body.at("schema").string() != schema ||
        body.canonical() != stored.body ||
        sha256_hex(stored.body) != stored.fingerprint ||
        stored.pair_id != stored.fingerprint)
        throw std::runtime_error("linked_shard_event_invalid");
    const auto& name = body.at("host").string();
    if (name != "codex" && name != "claude")
        throw std::runtime_error("linked_shard_host_invalid");
    const auto& key = body.at("session_key").string();
    require_digest(key);
    if (stored.request_id != "link:" + name + ":" + key)
        throw std::runtime_error("linked_shard_event_source_invalid");
    std::vector<LinkedSessionShard> result;
    std::set<std::string> ids;
    std::set<std::filesystem::path> paths;
    for (const auto& raw : body.at("shards").array()) {
        auto linked = parse_row(root, raw);
        if (host_name(linked.host) != name || linked.session_key != key ||
            !ids.insert(linked.seal.identifier).second ||
            !paths.insert(std::filesystem::canonical(
                linked.seal.directory)).second)
            throw std::runtime_error("linked_shard_event_duplicate");
        result.push_back(std::move(linked));
    }
    if (result.empty())
        throw std::runtime_error("linked_shard_event_empty");
    return result;
}

// SWEGCA: src/swegca_vrs2/linked_shards.py@c06092a:70-94
void require_native_registry(const std::filesystem::path& root) {
    if (std::filesystem::exists(root / "linked-shards.json"))
        throw std::runtime_error("linked_shard_legacy_registry_present");
}

// SWEGCA: src/swegca_vrs2/linked_shards.py@c06092a:70-94
std::filesystem::path registry_directory(const std::filesystem::path& root) {
    return root / "linked-shards-native";
}

}  // namespace

// SWEGCA: src/swegca_vrs2/linked_shards.py@c06092a:70-94
void visit_linked_sessions(
    const std::filesystem::path& state_root,
    const std::function<void(const LinkedSessionShard&)>& visit) {
    const auto root = std::filesystem::weakly_canonical(state_root);
    require_native_registry(root);
    const auto directory = registry_directory(root);
    if (!std::filesystem::exists(directory)) return;
    OwnerLock lock(directory / "owner.lock");
    lock.acquire(std::chrono::milliseconds(-1));
    require_native_registry(root);
    NativeJournal journal(directory, false, false);
    const auto limit = count(journal.row_count());
    journal.visit_rows(0, limit, [&](JournalRow&& stored) {
        for (const auto& linked : parse_link_event(root, stored))
            visit(linked);
    });
}

// SWEGCA: src/swegca_vrs2/linked_shards.py@c06092a:97-138
// SWEGCA: user@2026-09-22:72-79
SessionLinkReceipt attach_ended_session(
    const std::filesystem::path& state_root, SessionHost host,
    std::string_view session_id) {
    const auto root = std::filesystem::weakly_canonical(state_root);
    require_native_registry(root);
    const auto directory = registry_directory(root);
    OwnerLock lock(directory / "owner.lock");
    lock.acquire(std::chrono::milliseconds(-1));
    require_native_registry(root);
    const auto manifest = directory / "vrs-store.json";
    const bool create = !std::filesystem::exists(manifest);
    if (create) {
        for (const auto& entry : std::filesystem::directory_iterator(directory))
            if (entry.path().filename() != "owner.lock")
                throw std::runtime_error("linked_shard_registry_incomplete");
    }
    NativeJournal journal(directory, create, true, &lock);
    const auto key = sha256_hex(session_id);
    const auto seals = read_verified_ended_session(root, host, session_id);
    const auto event = link_event(root, host, key, seals);
    const auto bytes = event.canonical();
    const auto fingerprint = sha256_hex(bytes);
    std::set<std::string> candidate_ids;
    std::set<std::filesystem::path> candidate_paths;
    SessionLinkReceipt receipt{};
    receipt.shard_ids.reserve(seals.size());
    for (const auto& seal : seals) {
        receipt.shard_ids.push_back(seal.identifier);
        candidate_ids.insert(seal.identifier);
        candidate_paths.insert(std::filesystem::canonical(seal.directory));
        if (receipt.original_count >
                std::numeric_limits<std::uint64_t>::max() - seal.original_count ||
            receipt.cue_total >
                std::numeric_limits<std::uint64_t>::max() - seal.cue_total)
            throw std::runtime_error("linked_shard_count_overflow");
        receipt.original_count += seal.original_count;
        receipt.cue_total += seal.cue_total;
    }
    bool already_attached = false;
    journal.visit_rows(0, count(journal.row_count()),
        [&](JournalRow&& stored) {
            const auto previous = parse_link_event(root, stored);
            if (stored.request_id ==
                "link:" + host_name(host) + ":" + key) {
                if (already_attached || stored.fingerprint != fingerprint ||
                    stored.body != bytes)
                    throw std::runtime_error("linked_shard_reassignment_rejected");
                already_attached = true;
            }
            if (stored.fingerprint != fingerprint)
                for (const auto& row : previous)
                    if (candidate_ids.contains(row.seal.identifier) ||
                        candidate_paths.contains(std::filesystem::canonical(
                            row.seal.directory)))
                        throw std::runtime_error(
                            "linked_shard_reassignment_rejected");
        });
    if (!already_attached) {
        std::array<PendingJournalRow, 1> rows{PendingJournalRow{
            "link:" + host_name(host) + ":" + key,
            bytes, fingerprint, fingerprint}};
        const auto sequences = journal.append(rows);
        if (sequences.size() != 1)
            throw std::runtime_error("linked_shard_append_incomplete");
        receipt.added = seals.size();
    }
    return receipt;
}

}  // namespace swegca::vrs
