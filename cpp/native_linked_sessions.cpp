#include "native_linked_sessions.hpp"

#include "digest.hpp"
#include "journal_files.hpp"
#include "native_journal.hpp"
#include "owner_lock.hpp"

#include <chrono>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace swegca::vrs {
namespace {

constexpr std::string_view schema = "swegca-vrs2-native-linked-shards-v1";

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

// SWEGCA: src/swegca_vrs2/linked_shards.py@c06092a:70-94
std::vector<LinkedSessionShard> read_registry(const std::filesystem::path& root) {
    const auto path = root / "linked-shards.json";
    if (!std::filesystem::exists(path)) return {};
    if (!std::filesystem::is_regular_file(path))
        throw std::runtime_error("linked_shard_registry_invalid");
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("linked_shard_registry_invalid");
    const std::string bytes(std::istreambuf_iterator<char>{stream}, {});
    if (stream.bad()) throw std::runtime_error("linked_shard_registry_invalid");
    const auto body = Json::parse(bytes);
    if (body.object().size() != 2 || body.at("schema").string() != schema)
        throw std::runtime_error("linked_shard_registry_invalid");
    std::vector<LinkedSessionShard> result;
    std::set<std::string> ids;
    std::set<std::filesystem::path> paths;
    for (const auto& raw : body.at("shards").array()) {
        auto linked = parse_row(root, raw);
        if (!ids.insert(linked.seal.identifier).second ||
            !paths.insert(std::filesystem::canonical(
                linked.seal.directory)).second)
            throw std::runtime_error("linked_shard_registry_duplicate");
        result.push_back(std::move(linked));
    }
    return result;
}

// SWEGCA: src/swegca_vrs2/linked_shards.py@c06092a:97-138
void publish_registry(const std::filesystem::path& root,
                      const std::vector<LinkedSessionShard>& linked) {
    Json::Array rows;
    rows.reserve(linked.size());
    for (const auto& shard : linked)
        rows.push_back(serial_row(root, shard));
    Json::Object body;
    body.emplace("schema", Json(std::string(schema)));
    body.emplace("shards", Json(std::move(rows)));
    const auto bytes = Json(std::move(body)).canonical();
    write_atomic_file(root / "linked-shards.json",
                      std::as_bytes(std::span(bytes)));
}

}  // namespace

// SWEGCA: src/swegca_vrs2/linked_shards.py@c06092a:70-94
std::vector<LinkedSessionShard> load_linked_sessions(
    const std::filesystem::path& state_root) {
    return read_registry(std::filesystem::weakly_canonical(state_root));
}

// SWEGCA: src/swegca_vrs2/linked_shards.py@c06092a:97-138
// SWEGCA: user@2026-09-22:72-79
SessionLinkReceipt attach_ended_session(
    const std::filesystem::path& state_root, SessionHost host,
    std::string_view session_id) {
    const auto root = std::filesystem::weakly_canonical(state_root);
    OwnerLock lock(root / "linked-shards.json.lock");
    lock.acquire(std::chrono::milliseconds(-1));
    const auto key = sha256_hex(session_id);
    const auto seals = read_verified_ended_session(root, host, session_id);
    auto current = read_registry(root);
    std::map<std::string, Json> ids;
    std::map<std::filesystem::path, std::string> paths;
    for (const auto& row : current) {
        ids.emplace(row.seal.identifier, serial_row(root, row));
        paths.emplace(std::filesystem::canonical(row.seal.directory),
                      row.seal.identifier);
    }
    SessionLinkReceipt receipt{};
    receipt.shard_ids.reserve(seals.size());
    for (const auto& seal : seals) {
        LinkedSessionShard linked{host, key, seal};
        const auto serial = serial_row(root, linked);
        const auto directory = std::filesystem::canonical(seal.directory);
        receipt.shard_ids.push_back(seal.identifier);
        if (receipt.original_count >
                std::numeric_limits<std::uint64_t>::max() - seal.original_count ||
            receipt.cue_total >
                std::numeric_limits<std::uint64_t>::max() - seal.cue_total)
            throw std::runtime_error("linked_shard_count_overflow");
        receipt.original_count += seal.original_count;
        receipt.cue_total += seal.cue_total;
        if (const auto prior = ids.find(seal.identifier);
            prior != ids.end()) {
            if (prior->second.canonical() != serial.canonical())
                throw std::runtime_error("linked_shard_reassignment_rejected");
            continue;
        }
        if (paths.contains(directory))
            throw std::runtime_error("linked_shard_path_already_attached");
        ids.emplace(seal.identifier, serial);
        paths.emplace(directory, seal.identifier);
        current.push_back(std::move(linked));
        ++receipt.added;
    }
    if (receipt.added != 0)
        publish_registry(root, current);
    return receipt;
}

}  // namespace swegca::vrs
