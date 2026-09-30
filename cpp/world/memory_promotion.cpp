#include "world/memory_promotion.hpp"

#include "swegca_architecture/memory_promotion_kernel.hpp"
#include "swegca_architecture/sha256.hpp"
#include "world/semantic_vrs_ingress.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <utility>

extern "C" {
struct sqlite3;
struct sqlite3_stmt;
using sqlite3_int64 = long long;
using sqlite3_destructor_type = void (*)(void*);
int sqlite3_open_v2(const char*, sqlite3**, int, const char*);
int sqlite3_close(sqlite3*);
int sqlite3_exec(sqlite3*, const char*, int (*)(void*, int, char**, char**), void*, char**);
void sqlite3_free(void*);
const char* sqlite3_errmsg(sqlite3*);
int sqlite3_prepare_v2(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
int sqlite3_step(sqlite3_stmt*);
int sqlite3_finalize(sqlite3_stmt*);
int sqlite3_bind_text(sqlite3_stmt*, int, const char*, int, sqlite3_destructor_type);
int sqlite3_bind_int(sqlite3_stmt*, int, int);
int sqlite3_bind_int64(sqlite3_stmt*, int, sqlite3_int64);
int sqlite3_bind_double(sqlite3_stmt*, int, double);
int sqlite3_bind_null(sqlite3_stmt*, int);
const unsigned char* sqlite3_column_text(sqlite3_stmt*, int);
int sqlite3_column_int(sqlite3_stmt*, int);
sqlite3_int64 sqlite3_column_int64(sqlite3_stmt*, int);
double sqlite3_column_double(sqlite3_stmt*, int);
int sqlite3_column_type(sqlite3_stmt*, int);
int sqlite3_changes(sqlite3*);
}

namespace swegca::world {
namespace {

constexpr int sqlite_ok = 0;
constexpr int sqlite_row = 100;
constexpr int sqlite_done = 101;
constexpr int sqlite_open_readonly = 0x00000001;
constexpr int sqlite_open_readwrite = 0x00000002;
constexpr int sqlite_open_uri = 0x00000040;
constexpr int sqlite_null = 5;
auto sqlite_transient = reinterpret_cast<sqlite3_destructor_type>(-1);

std::string hex(const architecture::DigestBytes& digest) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result(digest.size() * 2, '0');
    for (std::size_t index = 0; index < digest.size(); ++index) {
        const auto value = std::to_integer<unsigned>(digest[index]);
        result[index * 2] = digits[value >> 4U];
        result[index * 2 + 1] = digits[value & 15U];
    }
    return result;
}

JsonValue aliases_json(const std::vector<std::string>& aliases) {
    JsonValue::Array result;
    for (const auto& alias : aliases) result.emplace_back(alias);
    return result;
}

sqlite3* connection(void* value) { return static_cast<sqlite3*>(value); }

[[noreturn]] void sqlite_failure(sqlite3* database, const std::string& operation) {
    throw std::runtime_error(operation + ": " +
                             (database ? sqlite3_errmsg(database) : "SQLite open failed"));
}

void execute(sqlite3* database, const char* sql) {
    char* message = nullptr;
    if (sqlite3_exec(database, sql, nullptr, nullptr, &message) != sqlite_ok) {
        const std::string detail = message ? message : sqlite3_errmsg(database);
        if (message) sqlite3_free(message);
        throw std::runtime_error("SQLite execute failed: " + detail);
    }
}

class Statement final {
public:
    Statement(sqlite3* database, const std::string& sql) : database_(database) {
        if (sqlite3_prepare_v2(database_, sql.c_str(), -1, &value_, nullptr) != sqlite_ok)
            sqlite_failure(database_, "SQLite prepare failed");
    }
    ~Statement() { if (value_) sqlite3_finalize(value_); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    sqlite3_stmt* get() const noexcept { return value_; }
    void text(const int index, const std::string_view value) {
        if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
            throw std::overflow_error("SQLite text parameter exceeds int");
        if (sqlite3_bind_text(value_, index, value.data(), static_cast<int>(value.size()),
                              sqlite_transient) != sqlite_ok)
            sqlite_failure(database_, "SQLite text bind failed");
    }
    void integer(const int index, const std::int64_t value) {
        if (sqlite3_bind_int64(value_, index, static_cast<sqlite3_int64>(value)) != sqlite_ok)
            sqlite_failure(database_, "SQLite integer bind failed");
    }
    void real(const int index, const double value) {
        if (sqlite3_bind_double(value_, index, value) != sqlite_ok)
            sqlite_failure(database_, "SQLite real bind failed");
    }
    void null(const int index) {
        if (sqlite3_bind_null(value_, index) != sqlite_ok)
            sqlite_failure(database_, "SQLite null bind failed");
    }
    int step() {
        const auto result = sqlite3_step(value_);
        if (result != sqlite_row && result != sqlite_done)
            sqlite_failure(database_, "SQLite step failed");
        return result;
    }
private:
    sqlite3* database_{};
    sqlite3_stmt* value_{};
};

std::string column_text(sqlite3_stmt* row, const int index) {
    const auto* value = sqlite3_column_text(row, index);
    return value ? reinterpret_cast<const char*>(value) : std::string{};
}
std::optional<std::string> optional_text(sqlite3_stmt* row, const int index) {
    if (sqlite3_column_type(row, index) == sqlite_null) return std::nullopt;
    return column_text(row, index);
}

void require_accept(const AccumulatorDecision& decision) {
    require_authoritative_accumulator_decision(decision);
    if (decision.status != "accept")
        throw AuthorityError("external memory mutation requires accepted evidence");
}

std::string utc_timestamp(const std::string_view input) {
    if (input.size() < 20 || input.back() != 'Z')
        throw std::invalid_argument("memory validity timestamps must use UTC Z format");
    const auto digit = [&](const std::size_t position) {
        if (position >= input.size() || input[position] < '0' || input[position] > '9')
            throw std::invalid_argument("invalid memory validity timestamp");
        return input[position] - '0';
    };
    if (input[4] != '-' || input[7] != '-' || (input[10] != 'T' && input[10] != ' ') ||
        input[13] != ':' || input[16] != ':')
        throw std::invalid_argument("invalid memory validity timestamp");
    const int year = digit(0) * 1000 + digit(1) * 100 + digit(2) * 10 + digit(3);
    const unsigned month = digit(5) * 10 + digit(6);
    const unsigned day = digit(8) * 10 + digit(9);
    const unsigned hour = digit(11) * 10 + digit(12);
    const unsigned minute = digit(14) * 10 + digit(15);
    const unsigned second = digit(17) * 10 + digit(18);
    if (!std::chrono::year_month_day(std::chrono::year(year), std::chrono::month(month),
                                     std::chrono::day(day)).ok() || hour > 23 || minute > 59 ||
        second > 59)
        throw std::invalid_argument("invalid memory validity timestamp");
    if (input.size() > 20) {
        if (input[19] != '.') throw std::invalid_argument("invalid memory validity timestamp");
        for (std::size_t index = 20; index + 1 < input.size(); ++index) (void)digit(index);
    }
    auto result = std::string(input.substr(0, 19)) + "Z";
    result[10] = 'T';
    return result;
}

JsonValue decision_payload(const AccumulatorDecision& decision) {
    JsonValue::Array addresses;
    for (const auto& address : decision.evidence_addresses) addresses.emplace_back(address);
    return JsonValue::Object{
        {"causal_lower_bound", decision.causal_lower_bound},
        {"context_diversity", JsonInteger{std::to_string(decision.context_diversity)}},
        {"effective_sample_size", decision.effective_sample_size},
        {"evidence_addresses", std::move(addresses)},
        {"hypothesis_id", decision.hypothesis_id},
        {"overall_upper_bound", decision.overall_upper_bound},
        {"posterior_mean", decision.posterior_mean},
        {"reason", decision.reason},
        {"regime_change_score", decision.regime_change_score},
        {"revision", JsonInteger{std::to_string(decision.revision)}},
        {"source_diversity", JsonInteger{std::to_string(decision.source_diversity)}},
        {"status", decision.status},
    };
}

std::string audit_payload(const std::optional<std::string>& supersedes,
                          const std::optional<std::string>& previous_until,
                          const AccumulatorDecision& decision) {
    JsonValue::Object result;
    result.emplace("decision", decision_payload(decision));
    result.emplace("previous_valid_until", previous_until ? JsonValue(*previous_until) : JsonValue(nullptr));
    result.emplace("supersedes_docid", supersedes ? JsonValue(*supersedes) : JsonValue(nullptr));
    return semantic_canonical_json(result);
}

}  // namespace

std::string_view memory_tier_name(const MemoryTier tier) noexcept {
    switch (tier) {
    case MemoryTier::none: return "none";
    case MemoryTier::episodic: return "episodic";
    case MemoryTier::semantic: return "semantic";
    case MemoryTier::quarantined: return "quarantined";
    case MemoryTier::retracted: return "retracted";
    }
    return "none";
}

MemoryCandidate::MemoryCandidate(
    std::string hypothesis, std::string key_value, std::string value_value,
    std::vector<std::string> references, std::string source,
    std::string revision, std::string timestamp_value,
    std::string license_value, std::string attribution_value,
    std::vector<std::string> aliases)
    : hypothesis_id(std::move(hypothesis)), key(std::move(key_value)),
      value(std::move(value_value)), evidence_refs(std::move(references)),
      source_id(std::move(source)), source_revision(std::move(revision)),
      timestamp(std::move(timestamp_value)), license(std::move(license_value)),
      attribution(std::move(attribution_value)) {
    if (!has_text(hypothesis_id) || !has_text(key) || !has_text(value) ||
        !has_text(source_id) || !has_text(source_revision) || !has_text(timestamp) ||
        !has_text(license) || !has_text(attribution))
        throw std::invalid_argument("memory candidate fields must be nonempty");
    if (evidence_refs.empty() ||
        std::ranges::any_of(evidence_refs, [](const auto& item) { return !has_text(item); }))
        throw std::invalid_argument("memory candidate requires provenance references");
    if (std::ranges::any_of(aliases, [](const auto& item) { return !has_text(item); }))
        throw std::invalid_argument("memory retrieval aliases cannot be empty");
    for (auto& alias : aliases)
        if (std::ranges::find(retrieval_aliases, alias) == retrieval_aliases.end())
            retrieval_aliases.push_back(std::move(alias));
}

MemoryPromotionDecision::MemoryPromotionDecision(
    const MemoryTier previous, const MemoryTier next, std::string action_value,
    std::string reason_value, const bool allowed, const bool authoritative)
    : previous_tier_(previous), next_tier_(next), action_(std::move(action_value)),
      reason_(std::move(reason_value)), semantic_read_allowed_(allowed),
      authoritative_(authoritative) {}
MemoryTier MemoryPromotionDecision::previous_tier() const noexcept { return previous_tier_; }
MemoryTier MemoryPromotionDecision::next_tier() const noexcept { return next_tier_; }
std::string_view MemoryPromotionDecision::action() const noexcept { return action_; }
std::string_view MemoryPromotionDecision::reason() const noexcept { return reason_; }
bool MemoryPromotionDecision::semantic_read_allowed() const noexcept { return semantic_read_allowed_; }

VRSExperiencePromotionDecision::VRSExperiencePromotionDecision(
    std::string snapshot, std::string connection_id_value,
    const double previous, const double current, std::string action_value,
    const bool is_promoted, const bool evidence_allowed,
    const bool experience_preserved, const bool action_permission,
    const bool write_permission)
    : snapshot_id(std::move(snapshot)), connection_id(std::move(connection_id_value)),
      previous_strength(previous), current_strength(current),
      action(std::move(action_value)), promoted(is_promoted),
      semantic_evidence_allowed(evidence_allowed),
      underlying_experience_preserved(experience_preserved),
      action_authorized(action_permission), persistent_write_authorized(write_permission) {
    if (!has_text(snapshot_id) || !has_text(connection_id))
        throw std::invalid_argument("VRS promotion requires snapshot and connection identities");
    if (!std::isfinite(previous_strength) || previous_strength < 0.0 ||
        !std::isfinite(current_strength) || current_strength < 0.0)
        throw std::invalid_argument("VRS promotion strengths must be finite and nonnegative");
    const bool was = previous_strength >= verified_experience_promotion_strength;
    const bool now = current_strength >= verified_experience_promotion_strength;
    const std::string expected = was && now ? "retain" : was ? "revoke" : now ? "promote" : "remain_unpromoted";
    if (action != expected || promoted != now || semantic_evidence_allowed != now)
        throw std::invalid_argument("VRS promotion decision changed");
    if (!underlying_experience_preserved || action_authorized || persistent_write_authorized)
        throw std::invalid_argument("VRS promotion changed its authority boundary");
}

VRSExperiencePromotionDecision assess_vrs_experience_promotion(
    std::string snapshot, std::string connection_id, const double previous,
    const double current) {
    const bool was = previous >= verified_experience_promotion_strength;
    const bool promoted = current >= verified_experience_promotion_strength;
    const std::string action = was && promoted ? "retain" : was ? "revoke" :
                               promoted ? "promote" : "remain_unpromoted";
    return {std::move(snapshot), std::move(connection_id), previous, current,
            action, promoted, promoted};
}

MemoryPromotionDecision decide_memory_promotion(
    const MemoryTier current, const AccumulatorDecision& evidence,
    const bool counterfactual_verified, const bool provenance_complete) {
    const auto authoritative = is_authoritative_accumulator_decision(evidence);
    if (!provenance_complete)
        return {current, MemoryTier::quarantined, "quarantine",
                "incomplete_provenance", false, authoritative};
    if (evidence.status == "reject")
        return {current, MemoryTier::retracted, "retract", evidence.reason,
                false, authoritative};
    if (evidence.status == "accept" && counterfactual_verified)
        return {current, MemoryTier::semantic,
                current == MemoryTier::semantic ? "refresh_semantic" : "promote",
                evidence.reason, true, authoritative};
    if (current == MemoryTier::semantic &&
        (evidence.reason == "regime_change_suspected" || !counterfactual_verified))
        return {current, MemoryTier::quarantined, "quarantine", evidence.reason,
                false, authoritative};
    return {current, MemoryTier::episodic, "record_episode", evidence.reason,
            false, authoritative};
}

void require_authoritative_promotion(const MemoryPromotionDecision& decision) {
    if (!decision.authoritative_)
        throw AuthorityError("memory promotion is not an authority capability");
}

MemoryCandidateDocument memory_candidate_document(
    const MemoryCandidate& candidate, const MemoryTier tier, std::string docid) {
    const auto tier_name = std::string(memory_tier_name(tier));
    std::string text = "FACT key=" + candidate.key + " value=" + candidate.value;
    if (!candidate.retrieval_aliases.empty())
        text += " aliases=" + semantic_canonical_json(aliases_json(candidate.retrieval_aliases));
    if (docid.empty()) docid = tier_name + ":" + candidate.hypothesis_id;
    auto title_tier = tier_name;
    if (!title_tier.empty()) title_tier[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(title_tier[0])));
    std::string refs;
    for (const auto& reference : candidate.evidence_refs) {
        if (!refs.empty()) refs += ',';
        refs += reference;
    }
    return {std::move(docid), candidate.source_id, candidate.source_revision,
            candidate.key, candidate.source_revision, tier_name + "_memory",
            candidate.timestamp, false, title_tier + " memory " + candidate.key,
            text, hex(architecture::Sha256::of(std::as_bytes(std::span(text.data(), text.size())))),
            "unique", "evidence://" + refs, candidate.license, candidate.attribution};
}

MosaicExternalMemory::MosaicExternalMemory(std::filesystem::path database,
                                           std::string tokenizer)
    : database_(std::filesystem::absolute(std::move(database))),
      tokenizer_(std::move(tokenizer)) {
    if (tokenizer_ != "trigram" && tokenizer_ != "unicode61")
        throw std::invalid_argument("unsupported tokenizer: " + tokenizer_);
    if (!std::filesystem::exists(database_))
        throw std::invalid_argument("external memory database does not exist: " + database_.string());
    database_ = std::filesystem::canonical(database_);
}
const std::filesystem::path& MosaicExternalMemory::database() const noexcept { return database_; }
std::string_view MosaicExternalMemory::tokenizer() const noexcept { return tokenizer_; }

void* MosaicExternalMemory::connect(const bool readonly) const {
    sqlite3* result = nullptr;
    const auto target = readonly ? "file:" + database_.generic_string() + "?mode=ro" : database_.string();
    const auto flags = readonly ? sqlite_open_readonly | sqlite_open_uri : sqlite_open_readwrite;
    if (sqlite3_open_v2(target.c_str(), &result, flags, nullptr) != sqlite_ok) {
        const auto message = result ? sqlite3_errmsg(result) : "SQLite open failed";
        const std::string copied(message);
        if (result) sqlite3_close(result);
        throw std::runtime_error(copied);
    }
    try {
        if (readonly) execute(result, "PRAGMA query_only=ON");
        else {
            execute(result, "PRAGMA foreign_keys=ON");
            ensure_update_triggers(result);
        }
    } catch (...) { sqlite3_close(result); throw; }
    return result;
}
void MosaicExternalMemory::close(void* value) noexcept { if (value) sqlite3_close(connection(value)); }

void MosaicExternalMemory::ensure_update_triggers(void* value) {
    execute(connection(value), R"SQL(
CREATE TRIGGER IF NOT EXISTS documents_after_insert AFTER INSERT ON documents BEGIN
 INSERT INTO documents_fts(rowid,title,text) VALUES(new.rowid,new.title,new.text); END;
CREATE TRIGGER IF NOT EXISTS documents_after_delete AFTER DELETE ON documents BEGIN
 INSERT INTO documents_fts(documents_fts,rowid,title,text) VALUES('delete',old.rowid,old.title,old.text); END;
CREATE TRIGGER IF NOT EXISTS documents_after_update AFTER UPDATE ON documents BEGIN
 INSERT INTO documents_fts(documents_fts,rowid,title,text) VALUES('delete',old.rowid,old.title,old.text);
 INSERT INTO documents_fts(rowid,title,text) VALUES(new.rowid,new.title,new.text); END;
CREATE TABLE IF NOT EXISTS document_resources(
 docid TEXT NOT NULL, resource_id TEXT NOT NULL, modality TEXT NOT NULL,
 storage_path TEXT NOT NULL, storage_sha256 TEXT NOT NULL, item_index INTEGER NOT NULL,
 metadata_json TEXT NOT NULL, PRIMARY KEY(docid,resource_id),
 FOREIGN KEY(docid) REFERENCES documents(docid) ON DELETE CASCADE);
CREATE INDEX IF NOT EXISTS document_resources_modality ON document_resources(modality);
)SQL");
}

void MosaicExternalMemory::refresh_duplicate_group(void* value,
                                                    const std::string_view digest) {
    auto* database = connection(value);
    Statement remove(database, "DELETE FROM duplicate_groups WHERE content_sha256=?");
    remove.text(1, digest); (void)remove.step();
    Statement count(database, "SELECT count(*) FROM documents WHERE content_sha256=?");
    count.text(1, digest);
    if (count.step() != sqlite_row) sqlite_failure(database, "duplicate count failed");
    const auto occurrences = sqlite3_column_int64(count.get(), 0);
    if (occurrences > 1) {
        Statement insert(database, "INSERT INTO duplicate_groups(content_sha256,occurrences) VALUES(?,?)");
        insert.text(1, digest); insert.integer(2, occurrences); (void)insert.step();
    }
}

void MosaicExternalMemory::upsert_in(void* value,
                                     const ExternalMemoryDocument& document) {
    auto* database = connection(value);
    std::optional<std::string> previous;
    {
        Statement existing(database, "SELECT content_sha256 FROM documents WHERE docid=?");
        existing.text(1, document.docid);
        if (existing.step() == sqlite_row) previous = column_text(existing.get(), 0);
    }
    Statement statement(database,
        "INSERT INTO documents(docid,source_id,source_revision,page_id,revision_id,namespace,timestamp,redirect,title,text,content_sha256,duplicate_status,source_url,license,attribution) "
        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(docid) DO UPDATE SET "
        "source_id=excluded.source_id,source_revision=excluded.source_revision,page_id=excluded.page_id,revision_id=excluded.revision_id,namespace=excluded.namespace,timestamp=excluded.timestamp,redirect=excluded.redirect,title=excluded.title,text=excluded.text,content_sha256=excluded.content_sha256,duplicate_status=excluded.duplicate_status,source_url=excluded.source_url,license=excluded.license,attribution=excluded.attribution");
    const std::string_view strings[]{document.docid, document.source_id, document.source_revision,
        document.page_id, document.revision_id, document.namespace_name, document.timestamp};
    for (int i = 0; i < 7; ++i) statement.text(i + 1, strings[i]);
    statement.integer(8, document.redirect ? 1 : 0);
    const std::string_view tail[]{document.title, document.text, document.content_sha256,
        document.duplicate_status, document.source_url, document.license, document.attribution};
    for (int i = 0; i < 7; ++i) statement.text(i + 9, tail[i]);
    (void)statement.step();
    refresh_duplicate_group(value, document.content_sha256);
    if (previous && *previous != document.content_sha256) refresh_duplicate_group(value, *previous);
}

void MosaicExternalMemory::upsert(const ExternalMemoryDocument& document) {
    void* raw = connect(false);
    try { execute(connection(raw), "BEGIN"); upsert_in(raw, document); execute(connection(raw), "COMMIT"); }
    catch (...) { close(raw); throw; }
    close(raw);
}

std::vector<ExternalMemoryDocument> MosaicExternalMemory::documents(
    const std::optional<std::string_view> namespace_name) const {
    void* raw = connect(true);
    try {
        auto* database = connection(raw);
        Statement statement(database,
            "SELECT docid,source_id,source_revision,page_id,revision_id,namespace,timestamp,redirect,title,text,content_sha256,duplicate_status,source_url,license,attribution FROM documents "
            + std::string(namespace_name ? "WHERE namespace=? " : "") + "ORDER BY timestamp,docid");
        if (namespace_name) statement.text(1, *namespace_name);
        std::vector<ExternalMemoryDocument> result;
        while (statement.step() == sqlite_row)
            result.push_back({column_text(statement.get(),0), column_text(statement.get(),1),
                column_text(statement.get(),2), column_text(statement.get(),3),
                column_text(statement.get(),4), column_text(statement.get(),5),
                column_text(statement.get(),6), sqlite3_column_int(statement.get(),7) != 0,
                column_text(statement.get(),8), column_text(statement.get(),9),
                column_text(statement.get(),10), column_text(statement.get(),11),
                column_text(statement.get(),12), column_text(statement.get(),13),
                column_text(statement.get(),14)});
        close(raw); return result;
    } catch (...) { close(raw); throw; }
}

bool MosaicExternalMemory::delete_in(void* value, const std::string_view docid) {
    auto* database = connection(value);
    std::optional<std::string> digest;
    {
        Statement existing(database, "SELECT content_sha256 FROM documents WHERE docid=?");
        existing.text(1, docid);
        if (existing.step() == sqlite_row) digest = column_text(existing.get(), 0);
    }
    if (!digest) return false;
    Statement remove(database, "DELETE FROM documents WHERE docid=?");
    remove.text(1, docid); (void)remove.step();
    refresh_duplicate_group(value, *digest);
    return true;
}

bool MosaicExternalMemory::delete_document(const std::string_view docid) {
    void* raw = connect(false);
    try {
        execute(connection(raw), "BEGIN");
        const auto deleted = delete_in(raw, docid);
        execute(connection(raw), "COMMIT"); close(raw); return deleted;
    } catch (...) { close(raw); throw; }
}

VersionedExternalMemory::VersionedExternalMemory(
    MosaicExternalMemory& memory, const std::size_t maximum_search_candidates)
    : memory_(memory), maximum_search_candidates_(maximum_search_candidates) {
    if (!maximum_search_candidates_)
        throw std::invalid_argument("maximum_search_candidates must be positive");
    ensure_schema();
}

void VersionedExternalMemory::ensure_schema() {
    void* raw = memory_.connect(false);
    try {
        execute(connection(raw), R"SQL(
BEGIN;
CREATE TABLE IF NOT EXISTS verified_document_validity(
 docid TEXT PRIMARY KEY, valid_from TEXT NOT NULL, valid_until TEXT,
 supersedes_docid TEXT, confidence REAL NOT NULL, evidence_revision INTEGER NOT NULL,
 update_id TEXT NOT NULL UNIQUE, FOREIGN KEY(docid) REFERENCES documents(docid) ON DELETE CASCADE);
CREATE INDEX IF NOT EXISTS verified_document_validity_window ON verified_document_validity(valid_from,valid_until);
CREATE TABLE IF NOT EXISTS verified_memory_audit(
 update_id TEXT PRIMARY KEY, operation TEXT NOT NULL, docid TEXT NOT NULL,
 applied_at TEXT NOT NULL, payload_json TEXT NOT NULL, rolled_back_at TEXT);
COMMIT;
)SQL");
        memory_.close(raw);
    } catch (...) { memory_.close(raw); throw; }
}

MemoryMutation VersionedExternalMemory::upsert_verified(
    const ExternalMemoryDocument& document, const AccumulatorDecision& decision,
    std::string valid_from, std::string update_id,
    std::optional<std::string> supersedes_docid,
    std::optional<std::string> valid_until) {
    require_accept(decision);
    valid_from = utc_timestamp(valid_from);
    if (valid_until) *valid_until = utc_timestamp(*valid_until);
    if (valid_until && *valid_until <= valid_from)
        throw std::invalid_argument("memory expiry must follow validity start");
    if (update_id.empty()) throw std::invalid_argument("update_id must be nonempty");
    void* raw = memory_.connect(false);
    auto* database = connection(raw);
    try {
        execute(database, "BEGIN IMMEDIATE");
        {
            Statement duplicate(database, "SELECT 1 FROM verified_memory_audit WHERE update_id=?");
            duplicate.text(1, update_id);
            if (duplicate.step() == sqlite_row) throw std::invalid_argument("update_id already exists");
        }
        {
            Statement duplicate(database, "SELECT 1 FROM verified_document_validity WHERE docid=?");
            duplicate.text(1, document.docid);
            if (duplicate.step() == sqlite_row) throw std::invalid_argument("versioned documents require a new docid");
        }
        std::optional<std::string> previous_until;
        if (supersedes_docid) {
            Statement predecessor(database,
                "SELECT valid_from,valid_until FROM verified_document_validity WHERE docid=?");
            predecessor.text(1, *supersedes_docid);
            if (predecessor.step() != sqlite_row)
                throw std::invalid_argument("superseded document is not verified");
            if (column_text(predecessor.get(), 0) > valid_from)
                throw std::invalid_argument("replacement predates superseded document");
            previous_until = optional_text(predecessor.get(), 1);
        }
        MosaicExternalMemory::upsert_in(raw, document);
        if (supersedes_docid) {
            Statement update(database,
                "UPDATE verified_document_validity SET valid_until=? WHERE docid=?");
            update.text(1, valid_from); update.text(2, *supersedes_docid); (void)update.step();
        }
        {
            Statement insert(database,
                "INSERT INTO verified_document_validity(docid,valid_from,valid_until,supersedes_docid,confidence,evidence_revision,update_id) VALUES(?,?,?,?,?,?,?)");
            insert.text(1, document.docid); insert.text(2, valid_from);
            if (valid_until) insert.text(3, *valid_until); else insert.null(3);
            if (supersedes_docid) insert.text(4, *supersedes_docid); else insert.null(4);
            insert.real(5, decision.posterior_mean);
            if (decision.revision > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
                throw std::overflow_error("evidence revision exceeds SQLite INTEGER");
            insert.integer(6, static_cast<std::int64_t>(decision.revision));
            insert.text(7, update_id); (void)insert.step();
        }
        {
            Statement audit(database,
                "INSERT INTO verified_memory_audit(update_id,operation,docid,applied_at,payload_json) VALUES(?,'upsert',?,?,?)");
            audit.text(1, update_id); audit.text(2, document.docid); audit.text(3, valid_from);
            audit.text(4, audit_payload(supersedes_docid, previous_until, decision));
            (void)audit.step();
        }
        execute(database, "COMMIT");
        memory_.close(raw);
        return {std::move(update_id), "upsert", document.docid,
                std::move(supersedes_docid), std::move(valid_from)};
    } catch (...) {
        try { execute(database, "ROLLBACK"); } catch (...) {}
        memory_.close(raw); throw;
    }
}

MemoryPromotionApplication apply_memory_promotion(
    MosaicExternalMemory& episodic, MosaicExternalMemory& semantic,
    const MemoryCandidate& candidate, const MemoryPromotionDecision& decision) {
    require_authoritative_promotion(decision);
    const auto episodic_docid = std::string(memory_tier_name(MemoryTier::episodic)) + ":" + candidate.hypothesis_id;
    const auto semantic_docid = std::string(memory_tier_name(MemoryTier::semantic)) + ":" + candidate.hypothesis_id;
    if (decision.next_tier() == MemoryTier::episodic) {
        episodic.upsert(memory_candidate_document(candidate, MemoryTier::episodic));
        (void)semantic.delete_document(semantic_docid);
    } else if (decision.next_tier() == MemoryTier::semantic) {
        for (const auto& document : semantic.documents("semantic_memory"))
            if (document.page_id == candidate.key && document.docid != semantic_docid)
                (void)semantic.delete_document(document.docid);
        semantic.upsert(memory_candidate_document(candidate, MemoryTier::semantic));
        (void)episodic.delete_document(episodic_docid);
    } else {
        (void)episodic.delete_document(episodic_docid);
        (void)semantic.delete_document(semantic_docid);
    }
    const auto episodic_documents = episodic.documents();
    const auto semantic_documents = semantic.documents();
    return {std::string(decision.action()), decision.next_tier(),
        std::ranges::any_of(episodic_documents, [&](const auto& row) { return row.docid == episodic_docid; }),
        std::ranges::any_of(semantic_documents, [&](const auto& row) { return row.docid == semantic_docid; }),
        decision.semantic_read_allowed()};
}

MemoryMutation apply_verified_memory_update(
    VersionedExternalMemory& memory, const MemoryCandidate& candidate,
    const AccumulatorDecision& evidence, const MemoryPromotionDecision& promotion,
    std::string valid_from, std::string update_id,
    std::optional<std::string> supersedes_docid,
    std::optional<std::string> valid_until) {
    require_authoritative_accumulator_decision(evidence);
    require_authoritative_promotion(promotion);
    if (promotion.next_tier() != MemoryTier::semantic ||
        !promotion.semantic_read_allowed())
        throw AuthorityError("candidate is not authorized for semantic memory");
    const auto docid = "semantic:" + candidate.hypothesis_id + ":" + candidate.source_revision;
    return memory.upsert_verified(
        memory_candidate_document(candidate, MemoryTier::semantic, docid), evidence,
        std::move(valid_from), std::move(update_id), std::move(supersedes_docid),
        std::move(valid_until));
}

}  // namespace swegca::world
