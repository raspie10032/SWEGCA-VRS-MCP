#include "native_operation_directory.hpp"

#include "digest.hpp"
#include "journal_files.hpp"
#include "json.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <zlib.h>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace swegca::vrs {
namespace {

constexpr std::string_view table_magic = "VRS2OPD1";
constexpr std::string_view text_magic = "VRS2OPT1";
constexpr std::string_view publication_name = "PUBLISHED.json";
constexpr std::string_view publication_schema = "swegca-vrs2-operation-directory-v1";
constexpr std::array<unsigned, 7> powers{12, 14, 16, 18, 20, 22, 23};
constexpr std::uint64_t header_bytes = 4096;
constexpr std::uint64_t text_header_bytes = 64;
constexpr std::uint64_t slot_bytes = 160;
constexpr std::uint64_t state_offset = 80;
constexpr std::uint32_t max_request_bytes = 4096;
using Key = std::array<unsigned char, 32>;
using Slot = std::array<unsigned char, slot_bytes>;

struct LevelState {
    std::uint64_t count;
    bool sealed;
};

struct StoredOperation {
    std::uint64_t request_offset;
    std::int64_t sequence;
    MainOperation operation;
};

// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:362-390
std::uint64_t get_u64(const unsigned char* bytes) {
    std::uint64_t result = 0;
    for (unsigned at = 0; at < 8; ++at)
        result |= std::uint64_t(bytes[at]) << (8 * at);
    return result;
}

// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:362-390
void put_u64(unsigned char* bytes, std::uint64_t value) {
    for (unsigned at = 0; at < 8; ++at)
        bytes[at] = static_cast<unsigned char>((value >> (8 * at)) & 0xff);
}

// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:362-390
std::uint32_t get_u32(const unsigned char* bytes) {
    std::uint32_t result = 0;
    for (unsigned at = 0; at < 4; ++at)
        result |= std::uint32_t(bytes[at]) << (8 * at);
    return result;
}

// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:362-390
void put_u32(unsigned char* bytes, std::uint32_t value) {
    for (unsigned at = 0; at < 4; ++at)
        bytes[at] = static_cast<unsigned char>((value >> (8 * at)) & 0xff);
}

// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:72-83
Key decode_digest(std::string_view value, std::string_view error) {
    if (value.size() != 64) throw std::runtime_error(std::string(error));
    Key result{};
    const auto digit = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        return -1;
    };
    for (std::size_t at = 0; at < result.size(); ++at) {
        const auto high = digit(value[at * 2]);
        const auto low = digit(value[at * 2 + 1]);
        if (high < 0 || low < 0) throw std::runtime_error(std::string(error));
        result[at] = static_cast<unsigned char>((high << 4) | low);
    }
    return result;
}

// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:72-83
std::string encode_digest(const unsigned char* bytes) {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (unsigned at = 0; at < 32; ++at) {
        result.push_back(digits[bytes[at] >> 4]);
        result.push_back(digits[bytes[at] & 15]);
    }
    return result;
}

// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:311-343
void read_exact(std::istream& stream, unsigned char* bytes, std::size_t size) {
    stream.read(reinterpret_cast<char*>(bytes), static_cast<std::streamsize>(size));
    if (stream.gcount() != static_cast<std::streamsize>(size))
        throw std::runtime_error("native_operation_file_truncated");
}

// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:311-343
void write_exact(std::ostream& stream, const unsigned char* bytes, std::size_t size) {
    stream.write(reinterpret_cast<const char*>(bytes),
                 static_cast<std::streamsize>(size));
    if (!stream) throw std::runtime_error("native_operation_write_failed");
}

// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:592-606
void sync_file(const std::filesystem::path& path) {
#if defined(_WIN32)
    const auto descriptor = _wopen(path.c_str(), _O_BINARY | _O_RDONLY);
    if (descriptor < 0) throw std::runtime_error("native_operation_sync_failed");
    const auto result = _commit(descriptor);
    _close(descriptor);
#else
    const auto descriptor = open(path.c_str(), O_RDONLY);
    if (descriptor < 0) throw std::runtime_error("native_operation_sync_failed");
    const auto result = fsync(descriptor);
    close(descriptor);
#endif
    if (result != 0) throw std::runtime_error("native_operation_sync_failed");
}

// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:583-609
std::optional<OperationDirectoryPublication> read_publication(
    const std::filesystem::path& directory) {
    const auto path = directory / publication_name;
    if (!std::filesystem::exists(path)) return std::nullopt;
    if (std::filesystem::file_size(path) > 4096)
        throw std::runtime_error("native_operation_publication_invalid");
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("native_operation_publication_invalid");
    const std::string bytes(std::istreambuf_iterator<char>{stream}, {});
    if (stream.bad()) throw std::runtime_error("native_operation_publication_invalid");
    try {
        const auto value = Json::parse(bytes);
        if (value.at("schema").string() != publication_schema)
            throw std::runtime_error("native_operation_publication_invalid");
        auto generation = value.at("journal_generation").string();
        auto rows = value.at("published_rows").integer();
        auto pair = value.at("pair_snapshot_id").string();
        if (generation.size() != 34 || !generation.starts_with("g-") ||
            rows < 0 || pair.empty())
            throw std::runtime_error("native_operation_publication_invalid");
        return OperationDirectoryPublication{
            std::move(generation), rows, std::move(pair)};
    } catch (const std::exception&) {
        throw std::runtime_error("native_operation_publication_invalid");
    }
}

// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:311-343
void create_table(const std::filesystem::path& path, unsigned power,
                  unsigned char prefix, std::string_view generation) {
    std::array<unsigned char, header_bytes> header{};
    std::copy(table_magic.begin(), table_magic.end(), header.begin());
    header[16] = prefix;
    header[17] = static_cast<unsigned char>(power);
    std::copy(generation.begin(), generation.end(), header.begin() + 32);
    write_atomic_file(path, std::as_bytes(std::span(header)));
    std::filesystem::resize_file(
        path, header_bytes + (std::uint64_t{1} << power) * slot_bytes);
    sync_file(path);
}

// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:311-343
LevelState read_table_header(std::istream& stream, const std::filesystem::path& path,
                             unsigned power, unsigned char prefix,
                             std::string_view generation) {
    if (std::filesystem::file_size(path) !=
        header_bytes + (std::uint64_t{1} << power) * slot_bytes)
        throw std::runtime_error("native_operation_table_invalid");
    std::array<unsigned char, 96> header{};
    stream.seekg(0);
    read_exact(stream, header.data(), header.size());
    if (!std::equal(table_magic.begin(), table_magic.end(), header.begin()) ||
        header[16] != prefix || header[17] != power ||
        !std::equal(generation.begin(), generation.end(), header.begin() + 32))
        throw std::runtime_error("native_operation_table_invalid");
    const auto count = get_u64(header.data() + state_offset);
    const auto sealed = header[state_offset + 8];
    if (count > (std::uint64_t{1} << power) || sealed > 1)
        throw std::runtime_error("native_operation_table_invalid");
    return LevelState{count, sealed != 0};
}

// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:362-390
Slot read_slot(std::istream& stream, std::uint64_t offset) {
    Slot bytes{};
    stream.seekg(static_cast<std::streamoff>(offset));
    read_exact(stream, bytes.data(), bytes.size());
    if (get_u32(bytes.data() + 144) !=
        crc32(0, reinterpret_cast<const Bytef*>(bytes.data()), 144) ||
        !std::all_of(bytes.begin() + 148, bytes.end(),
                     [](auto byte) { return byte == 0; }))
        throw std::runtime_error("native_operation_slot_corrupt");
    return bytes;
}

// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:311-390
std::pair<std::uint64_t, bool> probe(std::istream& stream, const Key& key,
                                     unsigned power) {
    const auto mask = (std::uint64_t{1} << power) - 1;
    const auto start = get_u64(key.data() + 1) & mask;
    const auto step = (get_u64(key.data() + 9) | 1) & mask;
    for (std::uint64_t count = 0; count <= mask; ++count) {
        const auto offset = header_bytes +
            ((start + count * step) & mask) * slot_bytes;
        std::array<unsigned char, 32> found{};
        stream.seekg(static_cast<std::streamoff>(offset));
        read_exact(stream, found.data(), found.size());
        if (std::all_of(found.begin(), found.end(),
                        [](auto byte) { return byte == 0; })) {
            std::array<unsigned char, slot_bytes - 32> rest{};
            read_exact(stream, rest.data(), rest.size());
            if (!std::all_of(rest.begin(), rest.end(),
                             [](auto byte) { return byte == 0; }))
                throw std::runtime_error("native_operation_slot_corrupt");
            return {offset, false};
        }
        (void)read_slot(stream, offset);
        if (found == key) return {offset, true};
    }
    throw std::runtime_error("native_operation_table_full");
}

// SWEGCA: src/swegca_vrs2/store.py@7536139:378-399
Slot encode_slot(const Key& key, std::uint64_t request_offset,
                 std::int64_t sequence, const MainOperation& operation) {
    Slot bytes{};
    std::copy(key.begin(), key.end(), bytes.begin());
    put_u64(bytes.data() + 32, request_offset);
    put_u64(bytes.data() + 40, static_cast<std::uint64_t>(sequence));
    const auto fingerprint = decode_digest(
        operation.fingerprint, "native_operation_fingerprint_invalid");
    const auto pair = decode_digest(
        operation.pair_snapshot_id, "native_operation_pair_invalid");
    std::copy(fingerprint.begin(), fingerprint.end(), bytes.begin() + 48);
    std::copy(pair.begin(), pair.end(), bytes.begin() + 80);
    if (!operation.episode_id.empty()) {
        if (!operation.episode_id.starts_with("memory:"))
            throw std::runtime_error("native_operation_episode_invalid");
        const auto episode = decode_digest(
            std::string_view(operation.episode_id).substr(7),
            "native_operation_episode_invalid");
        std::copy(episode.begin(), episode.end(), bytes.begin() + 112);
    }
    put_u32(bytes.data() + 144,
            crc32(0, reinterpret_cast<const Bytef*>(bytes.data()), 144));
    return bytes;
}

// SWEGCA: src/swegca_vrs2/store.py@7536139:378-399
StoredOperation decode_slot(const Slot& bytes) {
    const auto sequence = get_u64(bytes.data() + 40);
    if (sequence == 0 ||
        sequence > static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max()))
        throw std::runtime_error("native_operation_slot_corrupt");
    const bool has_episode = !std::all_of(
        bytes.begin() + 112, bytes.begin() + 144,
        [](auto byte) { return byte == 0; });
    return StoredOperation{
        get_u64(bytes.data() + 32),
        static_cast<std::int64_t>(sequence),
        MainOperation{encode_digest(bytes.data() + 48),
                      has_episode ? "memory:" + encode_digest(bytes.data() + 112)
                                  : std::string{},
                      encode_digest(bytes.data() + 80)}};
}

// SWEGCA: src/swegca_vrs2/cue_shards.py@c06092a:75-91
void create_text(const std::filesystem::path& path,
                 std::string_view generation) {
    std::array<unsigned char, text_header_bytes> header{};
    std::copy(text_magic.begin(), text_magic.end(), header.begin());
    std::copy(generation.begin(), generation.end(), header.begin() + 16);
    write_atomic_file(path, std::as_bytes(std::span(header)));
}

// SWEGCA: src/swegca_vrs2/cue_shards.py@c06092a:75-91
void require_text_header(std::istream& stream, std::string_view generation) {
    std::array<unsigned char, text_header_bytes> found{};
    stream.seekg(0);
    read_exact(stream, found.data(), found.size());
    std::array<unsigned char, text_header_bytes> expected{};
    std::copy(text_magic.begin(), text_magic.end(), expected.begin());
    std::copy(generation.begin(), generation.end(), expected.begin() + 16);
    if (found != expected)
        throw std::runtime_error("native_operation_text_header_invalid");
}

// SWEGCA: src/swegca_vrs2/cue_shards.py@c06092a:260-360
std::uint64_t append_request(const std::filesystem::path& path,
                             std::string_view request_id,
                             std::string_view generation, bool sync_now) {
    std::fstream stream(path, std::ios::in | std::ios::out | std::ios::binary);
    if (!stream) throw std::runtime_error("native_operation_text_invalid");
    require_text_header(stream, generation);
    stream.seekp(0, std::ios::end);
    const auto offset = static_cast<std::uint64_t>(stream.tellp());
    std::array<unsigned char, 4> size{};
    put_u32(size.data(), static_cast<std::uint32_t>(request_id.size()));
    write_exact(stream, size.data(), size.size());
    stream.write(request_id.data(), static_cast<std::streamsize>(request_id.size()));
    const auto crc = crc32(0,
        reinterpret_cast<const Bytef*>(request_id.data()),
        static_cast<uInt>(request_id.size()));
    std::array<unsigned char, 4> checksum{};
    put_u32(checksum.data(), crc);
    write_exact(stream, checksum.data(), checksum.size());
    stream.flush();
    if (!stream) throw std::runtime_error("native_operation_text_invalid");
    stream.close();
    if (sync_now) sync_file(path);
    return offset;
}

// SWEGCA: src/swegca_vrs2/cue_shards.py@c06092a:392-440
std::string read_request(const std::filesystem::path& path,
                         std::uint64_t offset, std::string_view generation) {
    const auto file_size = std::filesystem::file_size(path);
    if (offset < text_header_bytes || offset > file_size ||
        file_size - offset < 8)
        throw std::runtime_error("native_operation_text_invalid");
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("native_operation_text_invalid");
    require_text_header(stream, generation);
    stream.seekg(static_cast<std::streamoff>(offset));
    std::array<unsigned char, 4> size{};
    read_exact(stream, size.data(), size.size());
    const auto length = get_u32(size.data());
    if (length == 0 || length > max_request_bytes ||
        length > file_size - offset - 8)
        throw std::runtime_error("native_operation_text_invalid");
    std::string request(length, '\0');
    read_exact(stream, reinterpret_cast<unsigned char*>(request.data()), length);
    std::array<unsigned char, 4> checksum{};
    read_exact(stream, checksum.data(), checksum.size());
    if (get_u32(checksum.data()) !=
        crc32(0, reinterpret_cast<const Bytef*>(request.data()), length))
        throw std::runtime_error("native_operation_text_invalid");
    return request;
}

}  // namespace

// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:72-83
NativeOperationDirectory::Key NativeOperationDirectory::key_of(
    std::string_view request_id) {
    if (request_id.empty() || request_id.size() > max_request_bytes)
        throw std::runtime_error("native_operation_request_invalid");
    const auto key = decode_digest(
        sha256_hex(request_id), "native_operation_request_invalid");
    if (std::all_of(key.begin(), key.end(),
                    [](auto byte) { return byte == 0; }))
        throw std::runtime_error("native_operation_request_reserved");
    return key;
}

// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:311-343
std::filesystem::path NativeOperationDirectory::table_path(
    const Key& key, unsigned power) const {
    std::ostringstream name;
    name << "operation-p" << power << '-' << std::hex << std::setw(2)
         << std::setfill('0') << static_cast<unsigned>(key[0]) << ".vrs";
    return directory_ / name.str();
}

// SWEGCA: src/swegca_vrs2/cue_shards.py@c06092a:75-91
std::filesystem::path NativeOperationDirectory::text_path(const Key& key) const {
    std::ostringstream name;
    name << "operation-text-" << std::hex << std::setw(2)
         << std::setfill('0') << static_cast<unsigned>(key[0]) << ".vrs";
    return directory_ / name.str();
}

// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:190-223
NativeOperationDirectory::NativeOperationDirectory(
    std::filesystem::path directory, std::string journal_generation,
    std::int64_t published_row_limit, OwnerLock* owner_lock)
    : directory_(std::move(directory)),
      journal_generation_(std::move(journal_generation)),
      published_row_limit_(published_row_limit), owner_lock_(owner_lock) {
    if (journal_generation_.size() != 34 ||
        !journal_generation_.starts_with("g-") ||
        published_row_limit_ < 0)
        throw std::runtime_error("native_operation_generation_invalid");
    if (owner_lock_) {
        if (!owner_lock_->locked())
            throw std::runtime_error("native_vrs_owner_lock_required");
        if (std::filesystem::create_directories(directory_))
            std::filesystem::permissions(
                directory_, std::filesystem::perms::owner_all,
                std::filesystem::perm_options::replace);
    } else if (!std::filesystem::is_directory(directory_)) {
        throw std::runtime_error("native_operation_directory_missing");
    }
    publication_ = read_publication(directory_);
    if (publication_) {
        if (publication_->journal_generation != journal_generation_ ||
            published_row_limit_ > publication_->published_rows)
            throw std::runtime_error("native_operation_generation_changed");
    } else if (!owner_lock_ || !std::filesystem::is_empty(directory_)) {
        throw std::runtime_error("native_operation_unpublished_directory");
    }
}

// SWEGCA: src/swegca_vrs2/store.py@7536139:378-399
void NativeOperationDirectory::put(std::string_view request_id,
                                   const MainOperation& operation,
                                   std::int64_t journal_sequence) {
    const auto key = key_of(request_id);
    std::lock_guard guard(prefix_mutex_[key[0]]);
    if (failed_.load())
        throw std::runtime_error("native_operation_directory_failed");
    if (!owner_lock_ || !owner_lock_->locked())
        throw std::runtime_error("native_vrs_owner_lock_required");
    if (journal_sequence < 1)
        throw std::runtime_error("native_operation_sequence_invalid");
    (void)encode_slot(key, text_header_bytes, journal_sequence, operation);
    try {
        const auto text = text_path(key);
        if (!std::filesystem::exists(text))
            create_text(text, journal_generation_);
        bool sync_now;
        {
            std::lock_guard published(publication_mutex_);
            sync_now = publication_.has_value();
        }
        for (const auto power : powers) {
            const auto path = table_path(key, power);
            if (!std::filesystem::exists(path))
                create_table(path, power, key[0], journal_generation_);
            std::fstream stream(path, std::ios::in | std::ios::out | std::ios::binary);
            if (!stream) throw std::runtime_error("native_operation_table_invalid");
            auto state = read_table_header(
                stream, path, power, key[0], journal_generation_);
            const auto [offset, exists] = probe(stream, key, power);
            if (exists) {
                const auto stored = decode_slot(read_slot(stream, offset));
                if (read_request(text, stored.request_offset,
                                 journal_generation_) != request_id)
                    throw std::runtime_error("native_operation_digest_collision");
                if (stored.sequence != journal_sequence ||
                    stored.operation.fingerprint != operation.fingerprint ||
                    stored.operation.episode_id != operation.episode_id ||
                    stored.operation.pair_snapshot_id != operation.pair_snapshot_id)
                    throw std::runtime_error("native_operation_reassigned");
                return;
            }
            if (state.sealed) continue;
            const auto request_offset = append_request(
                text, request_id, journal_generation_, sync_now);
            const auto bytes = encode_slot(
                key, request_offset, journal_sequence, operation);
            stream.seekp(static_cast<std::streamoff>(offset + key.size()));
            write_exact(stream, bytes.data() + key.size(),
                        bytes.size() - key.size());
            stream.seekp(static_cast<std::streamoff>(offset));
            write_exact(stream, bytes.data(), key.size());
            ++state.count;
            state.sealed = state.count >=
                ((std::uint64_t{1} << power) * 7) / 10;
            std::array<unsigned char, 9> state_bytes{};
            put_u64(state_bytes.data(), state.count);
            state_bytes[8] = static_cast<unsigned char>(state.sealed);
            stream.seekp(static_cast<std::streamoff>(state_offset));
            write_exact(stream, state_bytes.data(), state_bytes.size());
            stream.flush();
            if (!stream) throw std::runtime_error("native_operation_write_failed");
            stream.close();
            if (sync_now) sync_file(path);
            return;
        }
        throw std::runtime_error("native_operation_directory_capacity_exceeded");
    } catch (...) {
        failed_.store(true);
        throw;
    }
}

// SWEGCA: src/swegca_vrs2/store.py@7536139:378-383
std::optional<MainOperation> NativeOperationDirectory::find_operation(
    std::string_view request_id) const {
    const auto key = key_of(request_id);
    std::lock_guard guard(prefix_mutex_[key[0]]);
    if (failed_.load())
        throw std::runtime_error("native_operation_directory_failed");
    if (!owner_lock_) {
        std::lock_guard published(publication_mutex_);
        if (!publication_ ||
            published_row_limit_ > publication_->published_rows)
            throw std::runtime_error("native_operation_unpublished_read");
    }
    for (const auto power : powers) {
        const auto path = table_path(key, power);
        if (!std::filesystem::exists(path)) return std::nullopt;
        std::ifstream stream(path, std::ios::binary);
        if (!stream) throw std::runtime_error("native_operation_table_invalid");
        const auto state = read_table_header(
            stream, path, power, key[0], journal_generation_);
        const auto [offset, exists] = probe(stream, key, power);
        if (exists) {
            const auto stored = decode_slot(read_slot(stream, offset));
            if (read_request(text_path(key), stored.request_offset,
                             journal_generation_) != request_id)
                throw std::runtime_error("native_operation_digest_collision");
            if (stored.sequence > published_row_limit_) return std::nullopt;
            return stored.operation;
        }
        if (!state.sealed) return std::nullopt;
    }
    return std::nullopt;
}

// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:583-609
void NativeOperationDirectory::publish(
    std::int64_t journal_rows, std::string_view pair_snapshot_id) {
    if (!owner_lock_ || !owner_lock_->locked())
        throw std::runtime_error("native_vrs_owner_lock_required");
    if (journal_rows < 0 || pair_snapshot_id.empty())
        throw std::runtime_error("native_operation_publication_invalid");
    std::vector<std::unique_lock<std::mutex>> locks;
    locks.reserve(prefix_mutex_.size());
    for (auto& mutex : prefix_mutex_) locks.emplace_back(mutex);
    if (failed_.load())
        throw std::runtime_error("native_operation_directory_failed");
    std::lock_guard published(publication_mutex_);
    if (publication_ && journal_rows < publication_->published_rows)
        throw std::runtime_error("native_operation_publication_regressed");
    try {
        for (const auto& entry : std::filesystem::directory_iterator(directory_))
            if (entry.is_regular_file() &&
                entry.path().filename() !=
                    std::filesystem::path(std::string(publication_name)))
                sync_file(entry.path());
        Json::Object body;
        body.emplace("schema", Json(std::string(publication_schema)));
        body.emplace("journal_generation", Json(journal_generation_));
        body.emplace("published_rows", Json(journal_rows));
        body.emplace("pair_snapshot_id", Json(std::string(pair_snapshot_id)));
        const auto bytes = Json(std::move(body)).canonical();
        write_atomic_file(
            directory_ / publication_name, std::as_bytes(std::span(bytes)));
        publication_ = OperationDirectoryPublication{
            journal_generation_, journal_rows, std::string(pair_snapshot_id)};
        published_row_limit_ = journal_rows;
    } catch (...) {
        failed_.store(true);
        throw;
    }
}

// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:583-609
std::optional<OperationDirectoryPublication>
NativeOperationDirectory::publication() const {
    std::lock_guard guard(publication_mutex_);
    return publication_;
}

// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:190-223
bool NativeOperationDirectory::fresh() const {
    if (failed_.load())
        throw std::runtime_error("native_operation_directory_failed");
    return std::filesystem::is_empty(directory_);
}

}  // namespace swegca::vrs
