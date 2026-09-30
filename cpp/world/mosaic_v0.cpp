#include "world/mosaic_v0.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <climits>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <type_traits>

namespace swegca::world {
namespace {

struct sqlite3;
struct sqlite3_stmt;
using SqliteDestructor = void (*)(void*);

constexpr int sqlite_ok = 0;
constexpr int sqlite_row = 100;
constexpr int sqlite_done = 101;
constexpr int sqlite_integer = 1;
constexpr int sqlite_open_readwrite = 0x00000002;
constexpr int sqlite_open_create = 0x00000004;

class SqliteApi final {
public:
    using OpenV2 = int (*)(const char*, sqlite3**, int, const char*);
    using CloseV2 = int (*)(sqlite3*);
    using BusyTimeout = int (*)(sqlite3*, int);
    using Exec = int (*)(sqlite3*, const char*, int (*)(void*, int, char**, char**), void*, char**);
    using Free = void (*)(void*);
    using PrepareV2 = int (*)(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
    using Finalize = int (*)(sqlite3_stmt*);
    using BindText = int (*)(sqlite3_stmt*, int, const char*, int, SqliteDestructor);
    using BindDouble = int (*)(sqlite3_stmt*, int, double);
    using BindInt64 = int (*)(sqlite3_stmt*, int, std::int64_t);
    using BindNull = int (*)(sqlite3_stmt*, int);
    using Step = int (*)(sqlite3_stmt*);
    using ColumnInt64 = std::int64_t (*)(sqlite3_stmt*, int);
    using ColumnText = const unsigned char* (*)(sqlite3_stmt*, int);
    using ColumnBytes = int (*)(sqlite3_stmt*, int);
    using ColumnDouble = double (*)(sqlite3_stmt*, int);
    using ColumnType = int (*)(sqlite3_stmt*, int);
    using LastInsertRowid = std::int64_t (*)(sqlite3*);
    using Changes = int (*)(sqlite3*);
    using Errmsg = const char* (*)(sqlite3*);

    SqliteApi() {
        library_ = ::dlopen("libsqlite3.so.0", RTLD_NOW | RTLD_LOCAL);
        if (library_ == nullptr) library_ = ::dlopen("libsqlite3.so", RTLD_NOW | RTLD_LOCAL);
        if (library_ == nullptr) throw std::runtime_error("SQLite runtime is unavailable");
        open_v2 = load<OpenV2>("sqlite3_open_v2");
        close_v2 = load<CloseV2>("sqlite3_close_v2");
        busy_timeout = load<BusyTimeout>("sqlite3_busy_timeout");
        exec = load<Exec>("sqlite3_exec");
        free = load<Free>("sqlite3_free");
        prepare_v2 = load<PrepareV2>("sqlite3_prepare_v2");
        finalize = load<Finalize>("sqlite3_finalize");
        bind_text = load<BindText>("sqlite3_bind_text");
        bind_double = load<BindDouble>("sqlite3_bind_double");
        bind_int64 = load<BindInt64>("sqlite3_bind_int64");
        bind_null = load<BindNull>("sqlite3_bind_null");
        step = load<Step>("sqlite3_step");
        column_int64 = load<ColumnInt64>("sqlite3_column_int64");
        column_text = load<ColumnText>("sqlite3_column_text");
        column_bytes = load<ColumnBytes>("sqlite3_column_bytes");
        column_double = load<ColumnDouble>("sqlite3_column_double");
        column_type = load<ColumnType>("sqlite3_column_type");
        last_insert_rowid = load<LastInsertRowid>("sqlite3_last_insert_rowid");
        changes = load<Changes>("sqlite3_changes");
        errmsg = load<Errmsg>("sqlite3_errmsg");
    }

    ~SqliteApi() { if (library_ != nullptr) ::dlclose(library_); }
    SqliteApi(const SqliteApi&) = delete;
    SqliteApi& operator=(const SqliteApi&) = delete;

    OpenV2 open_v2{};
    CloseV2 close_v2{};
    BusyTimeout busy_timeout{};
    Exec exec{};
    Free free{};
    PrepareV2 prepare_v2{};
    Finalize finalize{};
    BindText bind_text{};
    BindDouble bind_double{};
    BindInt64 bind_int64{};
    BindNull bind_null{};
    Step step{};
    ColumnInt64 column_int64{};
    ColumnText column_text{};
    ColumnBytes column_bytes{};
    ColumnDouble column_double{};
    ColumnType column_type{};
    LastInsertRowid last_insert_rowid{};
    Changes changes{};
    Errmsg errmsg{};

private:
    template <typename Function>
    [[nodiscard]] Function load(const char* name) {
        const auto symbol = ::dlsym(library_, name);
        if (symbol == nullptr) throw std::runtime_error(std::string("missing SQLite symbol: ") + name);
        static_assert(sizeof(Function) == sizeof(symbol));
        Function function{};
        std::memcpy(&function, &symbol, sizeof(function));
        return function;
    }
    void* library_{};
};

SqliteApi& sqlite_api() {
    static SqliteApi api;
    return api;
}

[[nodiscard]] std::string sqlite_error(sqlite3* database, const std::string_view operation) {
    const auto message = sqlite_api().errmsg(database);
    return std::string(operation) + ": " + (message == nullptr ? "unknown SQLite error" : message);
}

class SqliteConnection final {
public:
    explicit SqliteConnection(const std::filesystem::path& path) {
        const auto native_path = path.string();
        auto& api = sqlite_api();
        const auto result = api.open_v2(native_path.c_str(), &database_,
            sqlite_open_readwrite | sqlite_open_create, nullptr);
        if (result != sqlite_ok) {
            const auto message = database_ == nullptr ? std::string("unable to open SQLite database")
                                                      : sqlite_error(database_, "open SQLite database");
            if (database_ != nullptr) api.close_v2(database_);
            database_ = nullptr;
            throw std::runtime_error(message);
        }
        if (api.busy_timeout(database_, 5'000) != sqlite_ok) {
            const auto message = sqlite_error(database_, "set SQLite timeout");
            api.close_v2(database_);
            database_ = nullptr;
            throw std::runtime_error(message);
        }
    }

    ~SqliteConnection() { if (database_ != nullptr) sqlite_api().close_v2(database_); }
    SqliteConnection(const SqliteConnection&) = delete;
    SqliteConnection& operator=(const SqliteConnection&) = delete;
    [[nodiscard]] sqlite3* get() const noexcept { return database_; }

    void execute(const std::string_view sql) const {
        std::string statement(sql);
        char* detail = nullptr;
        const auto result = sqlite_api().exec(database_, statement.c_str(), nullptr, nullptr, &detail);
        if (result == sqlite_ok) return;
        std::string message = detail == nullptr ? sqlite_error(database_, "execute SQLite statement")
                                                : std::string(detail);
        if (detail != nullptr) sqlite_api().free(detail);
        throw std::runtime_error(message);
    }

private:
    sqlite3* database_{};
};

class SqliteStatement final {
public:
    SqliteStatement(sqlite3* database, const char* sql) : database_(database) {
        if (sqlite_api().prepare_v2(database_, sql, -1, &statement_, nullptr) != sqlite_ok)
            throw std::runtime_error(sqlite_error(database_, "prepare SQLite statement"));
    }
    ~SqliteStatement() { if (statement_ != nullptr) sqlite_api().finalize(statement_); }
    SqliteStatement(const SqliteStatement&) = delete;
    SqliteStatement& operator=(const SqliteStatement&) = delete;
    [[nodiscard]] sqlite3_stmt* get() const noexcept { return statement_; }

    void bind_text(const int index, const std::string_view value) const {
        if (value.size() > static_cast<std::size_t>(INT_MAX))
            throw std::length_error("SQLite text value is too large");
        const auto* data = value.empty() ? "" : value.data();
        if (sqlite_api().bind_text(statement_, index, data,
                static_cast<int>(value.size()), nullptr) != sqlite_ok)
            throw std::runtime_error(sqlite_error(database_, "bind SQLite text"));
    }
    void bind_double(const int index, const double value) const {
        if (sqlite_api().bind_double(statement_, index, value) != sqlite_ok)
            throw std::runtime_error(sqlite_error(database_, "bind SQLite real"));
    }
    void bind_int64(const int index, const std::int64_t value) const {
        if (sqlite_api().bind_int64(statement_, index, value) != sqlite_ok)
            throw std::runtime_error(sqlite_error(database_, "bind SQLite integer"));
    }
    void bind_null(const int index) const {
        if (sqlite_api().bind_null(statement_, index) != sqlite_ok)
            throw std::runtime_error(sqlite_error(database_, "bind SQLite null"));
    }
    [[nodiscard]] int step() const { return sqlite_api().step(statement_); }

private:
    sqlite3* database_{};
    sqlite3_stmt* statement_{};
};

void require_done(sqlite3* database, const int result, const std::string_view operation) {
    if (result != sqlite_done) throw std::runtime_error(sqlite_error(database, operation));
}

[[nodiscard]] std::string column_text(sqlite3_stmt* statement, const int column) {
    const auto* value = sqlite_api().column_text(statement, column);
    const auto bytes = sqlite_api().column_bytes(statement, column);
    if (bytes < 0 || (value == nullptr && bytes != 0))
        throw std::runtime_error("invalid SQLite text result");
    return value == nullptr ? std::string{} :
        std::string(reinterpret_cast<const char*>(value), static_cast<std::size_t>(bytes));
}

[[nodiscard]] bool python_whitespace(const std::uint32_t codepoint) noexcept {
    return (codepoint >= 0x0009U && codepoint <= 0x000DU) ||
        (codepoint >= 0x001CU && codepoint <= 0x0020U) || codepoint == 0x0085U ||
        codepoint == 0x00A0U || codepoint == 0x1680U ||
        (codepoint >= 0x2000U && codepoint <= 0x200AU) || codepoint == 0x2028U ||
        codepoint == 0x2029U || codepoint == 0x202FU || codepoint == 0x205FU ||
        codepoint == 0x3000U;
}

[[nodiscard]] bool has_non_whitespace(const std::string_view value) {
    std::size_t offset = 0;
    while (offset < value.size()) {
        const auto first = static_cast<unsigned char>(value[offset]);
        std::uint32_t codepoint{};
        std::size_t length{};
        if (first < 0x80U) { codepoint = first; length = 1; }
        else if (first >= 0xC2U && first <= 0xDFU) { codepoint = first & 0x1FU; length = 2; }
        else if (first >= 0xE0U && first <= 0xEFU) { codepoint = first & 0x0FU; length = 3; }
        else if (first >= 0xF0U && first <= 0xF4U) { codepoint = first & 0x07U; length = 4; }
        else return true;
        if (offset + length > value.size()) return true;
        for (std::size_t index = 1; index < length; ++index) {
            const auto continuation = static_cast<unsigned char>(value[offset + index]);
            if ((continuation & 0xC0U) != 0x80U) return true;
            codepoint = (codepoint << 6U) | (continuation & 0x3FU);
        }
        if ((length == 3 && codepoint < 0x800U) || (length == 4 && codepoint < 0x10000U) ||
            (codepoint >= 0xD800U && codepoint <= 0xDFFFU) || codepoint > 0x10FFFFU)
            return true;
        if (!python_whitespace(codepoint)) return true;
        offset += length;
    }
    return false;
}

[[nodiscard]] std::int64_t time_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

template <typename Operation>
decltype(auto) immediate_transaction(const SqliteConnection& connection, Operation operation) {
    connection.execute("BEGIN IMMEDIATE");
    try {
        if constexpr (std::is_void_v<std::invoke_result_t<Operation>>) {
            operation();
            connection.execute("COMMIT");
        } else {
            auto result = operation();
            connection.execute("COMMIT");
            return result;
        }
    } catch (...) {
        try { connection.execute("ROLLBACK"); } catch (...) {}
        throw;
    }
}

double distance(const MosaicVector& left, const MosaicVector& right) {
    if (left.size() != right.size()) throw std::invalid_argument("vector dimensions differ");
    double total = 0.0;
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto difference = left[index] - right[index];
        total += difference * difference;
    }
    return std::sqrt(total);
}

std::set<std::string, std::less<>> tokens(const std::string_view value) {
    std::set<std::string, std::less<>> result;
    std::string token;
    for (const unsigned char byte : value) {
        if (std::isalnum(byte) || byte == '_') token.push_back(static_cast<char>(std::tolower(byte)));
        else if (!token.empty()) { result.insert(std::move(token)); token.clear(); }
    }
    if (!token.empty()) result.insert(std::move(token));
    return result;
}

std::string lowercase(const std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const unsigned char byte : value)
        result.push_back(static_cast<char>(std::tolower(byte)));
    return result;
}

JsonValue::Array numbers(const MosaicVector& values) {
    JsonValue::Array result;
    for (const auto value : values) result.emplace_back(value);
    return result;
}

JsonValue::Array strings(const std::vector<std::string>& values) {
    JsonValue::Array result;
    for (const auto& value : values) result.emplace_back(value);
    return result;
}

}  // namespace

JsonValue::Object ConversationMemoryEvent::to_dict() const {
    return {{"id", id}, {"namespace", namespace_name}, {"kind", kind},
        {"subject", subject}, {"predicate", predicate}, {"value", value},
        {"source_turn", source_turn}, {"confidence", confidence},
        {"supersedes_id", supersedes_id.has_value() ? JsonValue(*supersedes_id) : JsonValue(nullptr)},
        {"created_ns", created_ns}};
}

ConversationMemory::ConversationMemory(std::filesystem::path path) : path_(std::move(path)) {
    const auto parent = path_.parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent);
    const SqliteConnection connection(path_);
    connection.execute(R"sql(
        PRAGMA journal_mode=WAL;
        PRAGMA synchronous=NORMAL;
        CREATE TABLE IF NOT EXISTS memory_events (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            namespace TEXT NOT NULL,
            kind TEXT NOT NULL,
            subject TEXT NOT NULL,
            predicate TEXT NOT NULL,
            value TEXT NOT NULL,
            source_turn TEXT NOT NULL,
            confidence REAL NOT NULL,
            supersedes_id INTEGER,
            created_ns INTEGER NOT NULL,
            FOREIGN KEY (supersedes_id) REFERENCES memory_events(id)
        );
        CREATE INDEX IF NOT EXISTS memory_lookup
            ON memory_events(namespace, kind, subject, predicate, id);
        CREATE INDEX IF NOT EXISTS memory_supersedes
            ON memory_events(supersedes_id);
    )sql");
}

std::int64_t ConversationMemory::remember_fact(const std::string_view namespace_name,
    const std::string_view subject, const std::string_view predicate,
    const std::string_view value, const std::string_view source_turn,
    const double confidence) const {
    for (const auto field : {namespace_name, subject, predicate, value, source_turn})
        if (!has_non_whitespace(field))
            throw std::invalid_argument("memory fact fields must not be empty");
    if (!(confidence >= 0.0 && confidence <= 1.0))
        throw std::invalid_argument("confidence must be between 0 and 1");

    const SqliteConnection connection(path_);
    return immediate_transaction(connection, [&]() {
        std::optional<std::int64_t> previous_id;
        std::string previous_value;
        {
            SqliteStatement previous(connection.get(), R"sql(
                SELECT id, value
                FROM memory_events
                WHERE namespace = ? AND kind = 'fact'
                  AND subject = ? AND predicate = ?
                ORDER BY id DESC
                LIMIT 1
            )sql");
            previous.bind_text(1, namespace_name);
            previous.bind_text(2, subject);
            previous.bind_text(3, predicate);
            const auto result = previous.step();
            if (result == sqlite_row) {
                previous_id = sqlite_api().column_int64(previous.get(), 0);
                previous_value = column_text(previous.get(), 1);
            } else if (result != sqlite_done) {
                throw std::runtime_error(sqlite_error(connection.get(), "read previous memory fact"));
            }
        }
        if (previous_id.has_value() && previous_value == value) return *previous_id;

        SqliteStatement insert(connection.get(), R"sql(
            INSERT INTO memory_events (
                namespace, kind, subject, predicate, value, source_turn,
                confidence, supersedes_id, created_ns
            ) VALUES (?, 'fact', ?, ?, ?, ?, ?, ?, ?)
        )sql");
        insert.bind_text(1, namespace_name);
        insert.bind_text(2, subject);
        insert.bind_text(3, predicate);
        insert.bind_text(4, value);
        insert.bind_text(5, source_turn);
        insert.bind_double(6, confidence);
        if (previous_id.has_value()) insert.bind_int64(7, *previous_id);
        else insert.bind_null(7);
        insert.bind_int64(8, time_ns());
        require_done(connection.get(), insert.step(), "insert memory fact");
        return sqlite_api().last_insert_rowid(connection.get());
    });
}

std::vector<ConversationMemoryEvent> ConversationMemory::active_facts(
    const std::string_view namespace_name) const {
    const SqliteConnection connection(path_);
    SqliteStatement query(connection.get(), R"sql(
        SELECT current.*
        FROM memory_events AS current
        WHERE current.namespace = ? AND current.kind = 'fact'
          AND NOT EXISTS (
              SELECT 1 FROM memory_events AS newer
              WHERE newer.supersedes_id = current.id
          )
        ORDER BY current.id
    )sql");
    query.bind_text(1, namespace_name);
    std::vector<ConversationMemoryEvent> rows;
    for (;;) {
        const auto result = query.step();
        if (result == sqlite_done) break;
        if (result != sqlite_row)
            throw std::runtime_error(sqlite_error(connection.get(), "read active memory facts"));
        const auto supersedes = sqlite_api().column_type(query.get(), 8) == sqlite_integer
            ? std::optional<std::int64_t>{sqlite_api().column_int64(query.get(), 8)}
            : std::nullopt;
        rows.push_back({sqlite_api().column_int64(query.get(), 0), column_text(query.get(), 1),
            column_text(query.get(), 2), column_text(query.get(), 3), column_text(query.get(), 4),
            column_text(query.get(), 5), column_text(query.get(), 6),
            sqlite_api().column_double(query.get(), 7), supersedes,
            sqlite_api().column_int64(query.get(), 9)});
    }
    return rows;
}

std::int64_t ConversationMemory::forget_fact(const std::string_view namespace_name,
    const std::string_view subject, const std::string_view predicate) const {
    for (const auto field : {namespace_name, subject, predicate})
        if (!has_non_whitespace(field))
            throw std::invalid_argument("memory fact identity fields must not be empty");

    const SqliteConnection connection(path_);
    return immediate_transaction(connection, [&]() {
        SqliteStatement remove(connection.get(), R"sql(
            DELETE FROM memory_events
            WHERE namespace = ? AND kind = 'fact'
              AND subject = ? AND predicate = ?
        )sql");
        remove.bind_text(1, namespace_name);
        remove.bind_text(2, subject);
        remove.bind_text(3, predicate);
        require_done(connection.get(), remove.step(), "delete memory facts");
        return std::max<std::int64_t>(0, sqlite_api().changes(connection.get()));
    });
}

void MosaicConfig::validate() const {
    if (!workspace_dim || !operator_top_k || !(halt_tolerance > 0) ||
        !(maximum_update_norm > 0))
        throw std::invalid_argument("MOSAIC v0 configuration must be positive");
}

void LowRankBasis::validate(const std::size_t dimension) const {
    if (name.empty()) throw std::invalid_argument("basis name must not be empty");
    if (left.size() != dimension || right.size() != dimension)
        throw std::invalid_argument("basis does not match workspace dimension");
}

void OperatorCode::validate() const {
    if (address.empty() || tags.empty() || coefficients.empty())
        throw std::invalid_argument("operator address, tags, and coefficients are required");
    if (confidence < 0 || confidence > 1)
        throw std::invalid_argument("operator confidence must be between 0 and 1");
}

JsonValue::Object MosaicRun::to_dict() const {
    JsonValue::Object coefficients;
    for (const auto& [basis, value] : operator_code.coefficients)
        coefficients.emplace(basis, value);
    JsonValue::Array conflicts;
    for (const auto& [left, right] : operator_code.conflicts)
        conflicts.emplace_back(JsonValue::Array{left, right});
    JsonValue::Array trace_rows;
    for (const auto& row : trace) trace_rows.emplace_back(row);
    return {{"success", success}, {"halted", halted}, {"stalled", stalled},
        {"steps", JsonInteger{std::to_string(steps)}}, {"final_state", numbers(final_state)},
        {"final_error", final_error}, {"operator", JsonValue::Object{
            {"coefficients", std::move(coefficients)}, {"selected", strings(operator_code.selected)},
            {"disabled", strings(operator_code.disabled)}, {"conflicts", std::move(conflicts)}}},
        {"trace", std::move(trace_rows)}};
}

OperatorArchive::OperatorArchive(std::vector<OperatorCode> operators)
    : operators_(std::move(operators)) {
    std::set<std::string, std::less<>> addresses;
    for (const auto& operation : operators_) {
        operation.validate();
        if (!addresses.insert(operation.address).second)
            throw std::invalid_argument("operator addresses must be unique");
    }
}

std::vector<OperatorCode> OperatorArchive::search(
    const std::string_view query, const std::size_t top_k) const {
    if (!top_k) return {};
    const auto query_tokens = tokens(query);
    std::vector<std::pair<std::size_t, const OperatorCode*>> ranked;
    for (const auto& operation : operators_) {
        std::size_t overlap = 0;
        for (const auto& tag : operation.tags)
            if (query_tokens.contains(lowercase(tag))) ++overlap;
        if (overlap) ranked.emplace_back(overlap, &operation);
    }
    std::ranges::sort(ranked, [](const auto& left, const auto& right) {
        if (left.first != right.first) return left.first > right.first;
        if (left.second->priority != right.second->priority)
            return left.second->priority > right.second->priority;
        if (left.second->confidence != right.second->confidence)
            return left.second->confidence > right.second->confidence;
        return left.second->address < right.second->address;
    });
    std::vector<OperatorCode> result;
    for (std::size_t index = 0; index < std::min(top_k, ranked.size()); ++index)
        result.push_back(*ranked[index].second);
    return result;
}

SynthesizedOperator synthesize_operators(const std::vector<OperatorCode>& candidates) {
    SynthesizedOperator result;
    std::vector<const OperatorCode*> selected;
    std::map<std::string, double, std::less<>> coefficients;
    for (const auto& candidate : candidates) {
        const auto blocker = std::ranges::find_if(selected, [&](const auto* active) {
            return active->conflicts.contains(candidate.address) ||
                candidate.conflicts.contains(active->address);
        });
        if (blocker != selected.end()) {
            result.disabled.push_back(candidate.address);
            result.conflicts.emplace_back((*blocker)->address, candidate.address);
            continue;
        }
        selected.push_back(&candidate);
        result.selected.push_back(candidate.address);
        for (const auto& [basis, coefficient] : candidate.coefficients)
            coefficients[basis] += coefficient;
    }
    result.coefficients.assign(coefficients.begin(), coefficients.end());
    return result;
}

RecurrentCell::RecurrentCell(std::vector<LowRankBasis> bases,
    const std::size_t dimension, const double maximum_update_norm)
    : dimension_(dimension), maximum_update_norm_(maximum_update_norm) {
    if (bases.empty()) throw std::invalid_argument("at least one operator basis is required");
    for (auto& basis : bases) {
        basis.validate(dimension_);
        if (!bases_.emplace(basis.name, std::move(basis)).second)
            throw std::invalid_argument("operator basis names must be unique");
    }
}

std::pair<MosaicVector, double> RecurrentCell::apply(
    const MosaicVector& state, const SynthesizedOperator& operation) const {
    if (state.size() != dimension_)
        throw std::invalid_argument("state does not match workspace dimension");
    MosaicVector delta(dimension_, 0.0);
    for (const auto& [basis_name, coefficient] : operation.coefficients) {
        const auto found = bases_.find(basis_name);
        if (found == bases_.end()) throw std::invalid_argument("unknown operator basis");
        const auto& basis = found->second;
        double activation = basis.bias;
        for (std::size_t index = 0; index < dimension_; ++index)
            activation += basis.right[index] * state[index];
        for (std::size_t index = 0; index < dimension_; ++index)
            delta[index] += coefficient * activation * basis.left[index];
    }
    double norm = 0.0;
    for (const auto value : delta) norm += value * value;
    norm = std::sqrt(norm);
    if (norm > maximum_update_norm_) {
        const auto scale = maximum_update_norm_ / norm;
        for (auto& value : delta) value *= scale;
        norm = maximum_update_norm_;
    }
    auto result = state;
    for (std::size_t index = 0; index < dimension_; ++index) result[index] += delta[index];
    return {std::move(result), norm};
}

MosaicSimulator::MosaicSimulator(MosaicConfig config,
    std::vector<LowRankBasis> bases, std::vector<OperatorCode> operators)
    : config_(config), archive_(std::move(operators)),
      cell_(std::move(bases), config.workspace_dim, config.maximum_update_norm) {
    config_.validate();
}

MosaicRun MosaicSimulator::solve(const std::string_view query,
    const MosaicVector& initial_state, const MosaicVector& goal,
    const std::size_t maximum_steps) const {
    if (initial_state.size() != config_.workspace_dim || goal.size() != initial_state.size())
        throw std::invalid_argument("initial state and goal must match workspace dimension");
    const auto operation = synthesize_operators(archive_.search(query, config_.operator_top_k));
    auto state = initial_state;
    auto error = distance(state, goal);
    std::vector<JsonValue::Object> trace;
    bool stalled = false;
    for (std::size_t step = 1; step <= maximum_steps; ++step) {
        if (error <= config_.halt_tolerance) break;
        const auto previous = state;
        auto [next, update_norm] = cell_.apply(state, operation);
        state = std::move(next);
        error = distance(state, goal);
        trace.push_back({{"step", JsonInteger{std::to_string(step)}},
            {"state", numbers(state)}, {"error", error}, {"update_norm", update_norm}});
        if (distance(previous, state) <= config_.halt_tolerance) { stalled = true; break; }
    }
    const bool success = error <= config_.halt_tolerance;
    return {success, success, stalled, trace.size(), std::move(state), error,
        operation, std::move(trace)};
}

JsonValue::Object run_mosaic_v0_demo(const std::filesystem::path& memory_path) {
    constexpr std::string_view namespace_name = "mosaic-v0";
    const ConversationMemory memory(memory_path);
    static_cast<void>(memory.remember_fact(namespace_name, "task", "target_steps", "3",
        "initial-pack"));
    static_cast<void>(memory.remember_fact(namespace_name, "task", "target_steps", "4",
        "replacement-pack"));
    const auto swapped_value = memory.active_facts(namespace_name).at(0).value;
    const auto deleted_rows = memory.forget_fact(namespace_name, "task", "target_steps");
    const auto deletion_ok = memory.active_facts(namespace_name).empty();
    MosaicSimulator simulator({},
        {{"progress", {1, 0, 0, 0}, {0, 0, 0, 0}, 1}},
        {{"procedure.forward", {"advance", "progress"}, {{"progress", 1}}, 1, 10,
             {"procedure.reverse"}},
         {"procedure.reverse", {"advance", "progress"}, {{"progress", -1}}, 1, 1,
             {"procedure.forward"}}});
    JsonValue::Array depths;
    std::vector<MosaicRun> runs;
    for (const std::size_t steps : {1U, 2U, 4U, 8U}) {
        auto run = simulator.solve("advance progress", {0, 0, 0, 0},
            {std::stod(swapped_value), 0, 0, 0}, steps);
        auto row = run.to_dict(); row.emplace("max_steps", JsonInteger{std::to_string(steps)});
        depths.emplace_back(std::move(row)); runs.push_back(std::move(run));
    }
    const auto unknown = simulator.solve("unseen operation", {0, 0, 0, 0},
        {std::stod(swapped_value), 0, 0, 0}, 8);
    JsonValue::Object acceptance{{"knowledge_swap", swapped_value == "4"},
        {"knowledge_delete", deletion_ok && deleted_rows == 2},
        {"operator_conflict", runs.back().operator_code.disabled ==
            std::vector<std::string>{"procedure.reverse"}},
        {"recurrent_scaling", !runs.front().success && runs[2].success},
        {"unknown_stalls_without_operator", unknown.stalled && unknown.operator_code.selected.empty()}};
    bool passed = true;
    for (const auto& [unused, value] : acceptance) {
        static_cast<void>(unused); passed = passed && std::get<bool>(value.storage());
    }
    JsonValue::Array active_after_delete;
    for (const auto& row : memory.active_facts(namespace_name))
        active_after_delete.emplace_back(row.to_dict());
    return {{"schema_version", "mosaic-v0"},
        {"scope", "deterministic architecture simulator; not an LM quality result"},
        {"memory", JsonValue::Object{{"swapped_value", swapped_value},
            {"deleted_rows", JsonInteger{std::to_string(deleted_rows)}},
            {"active_after_delete", std::move(active_after_delete)}}},
        {"recurrent_depths", std::move(depths)}, {"unknown_query", unknown.to_dict()},
        {"acceptance", std::move(acceptance)}, {"passed", passed}};
}

}  // namespace swegca::world
