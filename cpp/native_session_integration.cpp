#include "native_session_integration.hpp"

#include "digest.hpp"
#include "journal_files.hpp"
#include "native_journal.hpp"
#include "owner_lock.hpp"

#include <chrono>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace swegca::vrs {
namespace {

constexpr std::string_view cursor_schema =
    "swegca-vrs2-main-integration-cursor-v1";
constexpr std::uint64_t cursor_limit = 8192;

struct IntegrationCursor {
    std::uint64_t sequence = 0;
    std::string source_pair;
    std::string main_pair;
};

// SWEGCA: src/swegca_vrs2/linked_shards.py@c06092a:54-66
void require_digest(std::string_view value) {
    if (value.size() != 64)
        throw std::runtime_error("session_integration_digest_invalid");
    for (const char digit : value)
        if (!((digit >= '0' && digit <= '9') ||
              (digit >= 'a' && digit <= 'f')))
            throw std::runtime_error("session_integration_digest_invalid");
}

// SWEGCA: src/swegca_vrs2/session_capture.py@c06092a:123-150
std::int64_t checked_count(std::uint64_t value) {
    if (value > static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max()))
        throw std::runtime_error("session_integration_count_invalid");
    return static_cast<std::int64_t>(value);
}

// SWEGCA: src/swegca_vrs2/session_capture.py@c06092a:123-150
std::filesystem::path cursor_path(const std::filesystem::path& root,
                                   const LinkedSessionShard& linked) {
    return root / "main-integration" /
        (sha256_hex(linked.seal.identifier) + ".json");
}

// SWEGCA: src/swegca_vrs2/session_capture.py@c06092a:123-150
Json cursor_body(const LinkedSessionShard& linked,
                 const IntegrationCursor& cursor) {
    Json::Object body;
    body.emplace("schema", Json(std::string(cursor_schema)));
    body.emplace("id", Json(linked.seal.identifier));
    body.emplace("generation", Json(linked.seal.journal_generation));
    body.emplace("source_head_pair", Json(linked.seal.pair_snapshot_id));
    body.emplace("source_rows", Json(checked_count(linked.seal.journal_rows)));
    body.emplace("sequence", Json(checked_count(cursor.sequence)));
    body.emplace("source_pair", Json(cursor.source_pair));
    body.emplace("main_pair", Json(cursor.main_pair));
    return Json(std::move(body));
}

// SWEGCA: src/swegca_vrs2/session_capture.py@c06092a:123-150
IntegrationCursor read_cursor(const std::filesystem::path& path,
                              const LinkedSessionShard& linked) {
    if (!std::filesystem::exists(path)) return {};
    if (!std::filesystem::is_regular_file(path) ||
        std::filesystem::file_size(path) > cursor_limit)
        throw std::runtime_error("session_integration_cursor_invalid");
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("session_integration_cursor_invalid");
    const std::string bytes(std::istreambuf_iterator<char>{stream}, {});
    if (stream.bad()) throw std::runtime_error("session_integration_cursor_invalid");
    const auto body = Json::parse(bytes);
    if (body.object().size() != 8 ||
        body.at("schema").string() != cursor_schema ||
        body.at("id").string() != linked.seal.identifier ||
        body.at("generation").string() != linked.seal.journal_generation ||
        body.at("source_head_pair").string() !=
            linked.seal.pair_snapshot_id ||
        body.at("source_rows").integer() !=
            checked_count(linked.seal.journal_rows))
        throw std::runtime_error("session_integration_cursor_reassigned");
    const auto sequence = body.at("sequence").integer();
    if (sequence < 0 || static_cast<std::uint64_t>(sequence) >
                            linked.seal.journal_rows)
        throw std::runtime_error("session_integration_cursor_invalid");
    IntegrationCursor cursor{
        static_cast<std::uint64_t>(sequence),
        body.at("source_pair").string(),
        body.at("main_pair").string()};
    if (cursor.sequence == 0) {
        if (!cursor.source_pair.empty() || !cursor.main_pair.empty())
            throw std::runtime_error("session_integration_cursor_invalid");
    } else {
        require_digest(cursor.source_pair);
        require_digest(cursor.main_pair);
    }
    return cursor;
}

// SWEGCA: src/swegca_vrs2/session_capture.py@c06092a:123-150
void write_cursor(const std::filesystem::path& path,
                  const LinkedSessionShard& linked,
                  const IntegrationCursor& cursor) {
    const auto bytes = cursor_body(linked, cursor).canonical();
    if (bytes.size() > cursor_limit)
        throw std::runtime_error("session_integration_cursor_invalid");
    write_atomic_file(path, std::as_bytes(std::span(bytes)));
}

// SWEGCA: src/swegca_vrs2/linked_shards.py@c06092a:70-94
void require_registered(const std::filesystem::path& root,
                        const LinkedSessionShard& linked) {
    bool found = false;
    for (const auto& registered : load_linked_sessions(root)) {
        if (registered.seal.identifier != linked.seal.identifier) continue;
        if (registered.host != linked.host ||
            registered.session_key != linked.session_key ||
            registered.seal.directory != linked.seal.directory ||
            registered.seal.journal_generation !=
                linked.seal.journal_generation ||
            registered.seal.pair_snapshot_id !=
                linked.seal.pair_snapshot_id ||
            registered.seal.journal_rows != linked.seal.journal_rows ||
            registered.seal.original_count != linked.seal.original_count ||
            registered.seal.cue_total != linked.seal.cue_total)
            throw std::runtime_error("session_integration_source_reassigned");
        found = true;
        break;
    }
    if (!found)
        throw std::runtime_error("session_integration_source_unlinked");
}

}  // namespace

// SWEGCA: src/swegca_vrs2/linked_shards.py@c06092a:97-138
// SWEGCA: src/swegca_vrs2/store.py@c06092a:1389-1460
// SWEGCA: user@2026-09-22:72-79
SessionIntegrationReceipt integrate_linked_session_shard(
    const std::filesystem::path& state_root,
    const LinkedSessionShard& linked,
    MainVrsIntegrationSink& main) {
    const auto root = std::filesystem::weakly_canonical(state_root);
    require_registered(root, linked);
    OwnerLock worker(cursor_path(root, linked).string() + ".lock");
    worker.acquire(std::chrono::milliseconds(-1));
    OwnerLock source(linked.seal.directory / "owner.lock");
    source.acquire(std::chrono::milliseconds(0));
    NativeJournal journal(linked.seal.directory, false, false);
    if (journal.generation() != linked.seal.journal_generation ||
        journal.row_count() != linked.seal.journal_rows)
        throw std::runtime_error("session_integration_source_changed");
    const auto head = journal.head();
    if ((head && head->second != linked.seal.pair_snapshot_id) ||
        (!head && linked.seal.journal_rows != 0))
        throw std::runtime_error("session_integration_source_changed");
    const auto path = cursor_path(root, linked);
    auto cursor = read_cursor(path, linked);
    if (cursor.sequence != 0 &&
        journal.pair(checked_count(cursor.sequence)) != cursor.source_pair)
        throw std::runtime_error("session_integration_cursor_source_changed");
    SessionIntegrationReceipt receipt{
        linked.seal.identifier, 0, cursor.sequence, cursor.main_pair};
    if (cursor.sequence == linked.seal.journal_rows) return receipt;
    bool open = false;
    std::string group_pair;
    std::int64_t last_sequence = checked_count(cursor.sequence);
    const auto commit_group = [&] {
        const auto main_pair = main.commit_group();
        open = false;
        require_digest(main_pair);
        cursor.sequence = static_cast<std::uint64_t>(last_sequence);
        cursor.source_pair = group_pair;
        cursor.main_pair = main_pair;
        write_cursor(path, linked, cursor);
        receipt.final_sequence = cursor.sequence;
        receipt.last_main_pair_id = main_pair;
    };
    try {
        journal.visit_rows(checked_count(cursor.sequence),
            checked_count(linked.seal.journal_rows), [&](JournalRow&& row) {
                if (row.sequence != last_sequence + 1)
                    throw std::runtime_error("session_integration_sequence_gap");
                require_digest(row.pair_id);
                auto entry = parse_native_journal_entry(
                    row.request_id, row.body, row.fingerprint);
                if (open && row.pair_id != group_pair)
                    commit_group();
                if (!open) {
                    group_pair = row.pair_id;
                    main.begin_group(linked, group_pair);
                    open = true;
                }
                main.stage(row.request_id, row.sequence, std::move(entry));
                last_sequence = row.sequence;
                ++receipt.applied_rows;
            });
        if (open) commit_group();
    } catch (...) {
        if (open) main.abort_group();
        throw;
    }
    if (cursor.sequence != linked.seal.journal_rows)
        throw std::runtime_error("session_integration_tail_incomplete");
    return receipt;
}

}  // namespace swegca::vrs
