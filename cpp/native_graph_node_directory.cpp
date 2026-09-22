#include "native_graph_node_directory.hpp"

#include "digest.hpp"
#include "journal_files.hpp"
#include "json.hpp"
#include "memory_vrs_pair.hpp"
#include "native_journal_entry.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
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

constexpr std::string_view names_magic = "VRS2GNN1";
constexpr std::string_view reverse_magic = "VRS2GNR1";
constexpr std::string_view table_magic = "VRS2GNT1";
constexpr std::string_view publication_name = "PUBLISHED.json";
constexpr std::string_view publication_schema = "swegca-vrs2-graph-node-v1";
constexpr std::string_view indexed_name = "INDEXED.json";
constexpr std::string_view indexed_schema = "swegca-vrs2-graph-node-indexed-v1";
constexpr std::uint64_t file_header_bytes = 64;
constexpr std::uint64_t slot_bytes = 48;
constexpr std::uint64_t reverse_bytes = 16;
constexpr std::uint32_t maximum_name_bytes = 1024 * 1024;
constexpr std::array<unsigned, 7> powers{12, 14, 16, 18, 20, 22, 23};
using Key = std::array<unsigned char, 32>;
using Slot = std::array<unsigned char, slot_bytes>;

struct LevelState {
    std::uint64_t count;
    bool sealed;
};

// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:362-390
std::uint32_t get_u32(const unsigned char* bytes) {
    std::uint32_t value = 0;
    for (unsigned at = 0; at < 4; ++at)
        value |= std::uint32_t(bytes[at]) << (8 * at);
    return value;
}

// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:362-390
std::uint64_t get_u64(const unsigned char* bytes) {
    std::uint64_t value = 0;
    for (unsigned at = 0; at < 8; ++at)
        value |= std::uint64_t(bytes[at]) << (8 * at);
    return value;
}

// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:362-390
void put_u32(unsigned char* bytes, std::uint32_t value) {
    for (unsigned at = 0; at < 4; ++at)
        bytes[at] = static_cast<unsigned char>(value >> (8 * at));
}

// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:362-390
void put_u64(unsigned char* bytes, std::uint64_t value) {
    for (unsigned at = 0; at < 8; ++at)
        bytes[at] = static_cast<unsigned char>(value >> (8 * at));
}

// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:72-83
Key key_of(std::string_view name) {
    if (name.empty() || name.size() > maximum_name_bytes)
        throw std::runtime_error("graph_node_name_invalid");
    const bool record = name.size() == 71 && name.starts_with("memory:") &&
        std::all_of(name.begin() + 7, name.end(), [](char ch) {
            return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
        });
    if (!record && !(name.starts_with("cue:") && name.size() > 4))
        throw std::runtime_error("graph_node_name_invalid");
    const auto hex = sha256_hex(name);
    Key key{};
    const auto digit = [](char value) -> unsigned char {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        throw std::runtime_error("graph_node_name_invalid");
    };
    for (std::size_t at = 0; at < key.size(); ++at)
        key[at] = static_cast<unsigned char>(
            (digit(hex[2 * at]) << 4) | digit(hex[2 * at + 1]));
    if (std::all_of(key.begin(), key.end(),
                    [](auto byte) { return byte == 0; }))
        throw std::runtime_error("graph_node_name_reserved");
    return key;
}

// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:311-343
void read_exact(std::istream& stream, unsigned char* bytes, std::size_t size) {
    stream.read(reinterpret_cast<char*>(bytes), static_cast<std::streamsize>(size));
    if (stream.gcount() != static_cast<std::streamsize>(size))
        throw std::runtime_error("graph_node_file_truncated");
}

// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:311-343
void write_exact(std::ostream& stream, const unsigned char* bytes,
                 std::size_t size) {
    stream.write(reinterpret_cast<const char*>(bytes),
                 static_cast<std::streamsize>(size));
    if (!stream) throw std::runtime_error("graph_node_write_failed");
}

// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:592-606
void sync_file(const std::filesystem::path& path) {
#if defined(_WIN32)
    const auto descriptor = _wopen(path.c_str(), _O_BINARY | _O_RDONLY);
    if (descriptor < 0) throw std::runtime_error("graph_node_sync_failed");
    const auto result = _commit(descriptor);
    _close(descriptor);
#else
    const auto descriptor = open(path.c_str(), O_RDONLY);
    if (descriptor < 0) throw std::runtime_error("graph_node_sync_failed");
    const auto result = fsync(descriptor);
    close(descriptor);
#endif
    if (result != 0) throw std::runtime_error("graph_node_sync_failed");
}

// SWEGCA: src/swegca_vrs2/cue_shards.py@c06092a:75-91
void create_data_file(const std::filesystem::path& path,
                      std::string_view magic, std::string_view generation) {
    std::array<unsigned char, file_header_bytes> header{};
    std::copy(magic.begin(), magic.end(), header.begin());
    std::copy(generation.begin(), generation.end(), header.begin() + 16);
    write_atomic_file(path, std::as_bytes(std::span(header)));
}

// SWEGCA: src/swegca_vrs2/cue_shards.py@c06092a:75-91
void require_data_file(std::istream& stream, std::string_view magic,
                       std::string_view generation) {
    std::array<unsigned char, file_header_bytes> found{};
    stream.seekg(0);
    read_exact(stream, found.data(), found.size());
    std::array<unsigned char, file_header_bytes> expected{};
    std::copy(magic.begin(), magic.end(), expected.begin());
    std::copy(generation.begin(), generation.end(), expected.begin() + 16);
    if (found != expected)
        throw std::runtime_error("graph_node_data_header_invalid");
}

// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:311-343
void create_table(const std::filesystem::path& path, unsigned power,
                  unsigned char prefix, std::string_view generation) {
    std::array<unsigned char, file_header_bytes> header{};
    std::copy(table_magic.begin(), table_magic.end(), header.begin());
    std::copy(generation.begin(), generation.end(), header.begin() + 16);
    header[52] = prefix;
    header[53] = static_cast<unsigned char>(power);
    write_atomic_file(path, std::as_bytes(std::span(header)));
    std::filesystem::resize_file(
        path, file_header_bytes + (std::uint64_t{1} << power) * slot_bytes);
    sync_file(path);
}

// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:311-390
LevelState read_table_header(std::istream& stream,
                             const std::filesystem::path& path,
                             unsigned power, unsigned char prefix,
                             std::string_view generation) {
    if (std::filesystem::file_size(path) !=
        file_header_bytes + (std::uint64_t{1} << power) * slot_bytes)
        throw std::runtime_error("graph_node_table_invalid");
    std::array<unsigned char, file_header_bytes> header{};
    stream.seekg(0);
    read_exact(stream, header.data(), header.size());
    if (!std::equal(table_magic.begin(), table_magic.end(), header.begin()) ||
        !std::equal(generation.begin(), generation.end(), header.begin() + 16) ||
        header[52] != prefix || header[53] != power)
        throw std::runtime_error("graph_node_table_invalid");
    const auto count = get_u64(header.data() + 56);
    if (count > (std::uint64_t{1} << power) || header[54] > 1)
        throw std::runtime_error("graph_node_table_invalid");
    return LevelState{count, header[54] != 0};
}

// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:362-390
Slot read_slot(std::istream& stream, std::uint64_t offset) {
    Slot slot{};
    stream.seekg(static_cast<std::streamoff>(offset));
    read_exact(stream, slot.data(), slot.size());
    if (get_u32(slot.data() + 44) !=
        crc32(0, reinterpret_cast<const Bytef*>(slot.data()), 44))
        throw std::runtime_error("graph_node_slot_corrupt");
    return slot;
}

// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:311-390
std::pair<std::uint64_t, bool> probe(std::istream& stream, const Key& key,
                                     unsigned power) {
    const auto mask = (std::uint64_t{1} << power) - 1;
    const auto start = get_u64(key.data() + 1) & mask;
    const auto step = (get_u64(key.data() + 9) | 1) & mask;
    for (std::uint64_t count = 0; count <= mask; ++count) {
        const auto offset = file_header_bytes +
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
                throw std::runtime_error("graph_node_slot_corrupt");
            return {offset, false};
        }
        (void)read_slot(stream, offset);
        if (found == key) return {offset, true};
    }
    throw std::runtime_error("graph_node_table_full");
}

// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:583-609
std::optional<GraphNodePublication> read_publication(
    const std::filesystem::path& directory) {
    const auto path = directory / publication_name;
    if (!std::filesystem::exists(path)) return std::nullopt;
    if (std::filesystem::file_size(path) > 4096)
        throw std::runtime_error("graph_node_publication_invalid");
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("graph_node_publication_invalid");
    const std::string bytes(std::istreambuf_iterator<char>{stream}, {});
    if (stream.bad()) throw std::runtime_error("graph_node_publication_invalid");
    try {
        const auto value = Json::parse(bytes);
        if (value.at("schema").string() != publication_schema)
            throw std::runtime_error("graph_node_publication_invalid");
        auto generation = value.at("journal_generation").string();
        auto graph = value.at("graph_snapshot_id").string();
        auto pair = value.at("pair_snapshot_id").string();
        const auto rows = value.at("published_rows").integer();
        const auto count = value.at("node_count").integer();
        if (generation.size() != 34 || !generation.starts_with("g-") ||
            graph.size() != 64 || pair.size() != 64 || rows < 0 ||
            count < 0 || static_cast<std::uint64_t>(count) >
                             std::uint64_t{0x100000000ULL})
            throw std::runtime_error("graph_node_publication_invalid");
        return GraphNodePublication{std::move(generation), std::move(graph),
                                    std::move(pair), rows,
                                    static_cast<std::uint64_t>(count)};
    } catch (const std::exception&) {
        throw std::runtime_error("graph_node_publication_invalid");
    }
}

// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:583-609
std::uint64_t read_indexed(const std::filesystem::path& directory,
                           std::string_view generation) {
    const auto path = directory / indexed_name;
    if (!std::filesystem::exists(path)) return 0;
    if (std::filesystem::file_size(path) > 4096)
        throw std::runtime_error("graph_node_indexed_invalid");
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("graph_node_indexed_invalid");
    const std::string bytes(std::istreambuf_iterator<char>{stream}, {});
    if (stream.bad()) throw std::runtime_error("graph_node_indexed_invalid");
    try {
        const auto value = Json::parse(bytes);
        const auto count = value.at("node_count").integer();
        if (value.at("schema").string() != indexed_schema ||
            value.at("journal_generation").string() != generation ||
            count < 0 || static_cast<std::uint64_t>(count) >
                             std::uint64_t{0x100000000ULL})
            throw std::runtime_error("graph_node_indexed_invalid");
        return static_cast<std::uint64_t>(count);
    } catch (const std::exception&) {
        throw std::runtime_error("graph_node_indexed_invalid");
    }
}

// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:583-609
void write_indexed(const std::filesystem::path& directory,
                   std::string_view generation, std::uint64_t count) {
    if (count > std::uint64_t{0x100000000ULL} ||
        count < read_indexed(directory, generation))
        throw std::runtime_error("graph_node_indexed_regressed");
    Json::Object body;
    body.emplace("schema", Json(std::string(indexed_schema)));
    body.emplace("journal_generation", Json(std::string(generation)));
    body.emplace("node_count", Json(static_cast<std::int64_t>(count)));
    const auto bytes = Json(std::move(body)).canonical();
    write_atomic_file(directory / indexed_name,
                      std::as_bytes(std::span(bytes)));
}

}  // namespace

// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:311-343
std::filesystem::path NativeGraphNodeDirectory::table_path(
    unsigned power, unsigned char prefix) const {
    std::ostringstream name;
    name << "node-p" << power << '-' << std::hex << std::setw(2)
         << std::setfill('0') << static_cast<unsigned>(prefix) << ".vrs";
    return directory_ / name.str();
}

// SWEGCA: src/swegca_vrs2/store.py@c06092a:202-265
NativeGraphNodeDirectory::NativeGraphNodeDirectory(
    std::filesystem::path directory, std::string journal_generation,
    std::string graph_snapshot_id, std::uint64_t node_count,
    std::int64_t published_rows, OwnerLock* owner_lock)
    : directory_(std::move(directory)),
      journal_generation_(std::move(journal_generation)),
      graph_snapshot_id_(std::move(graph_snapshot_id)),
      node_count_(node_count), published_rows_(published_rows),
      owner_lock_(owner_lock) {
    if (journal_generation_.size() != 34 ||
        !journal_generation_.starts_with("g-") ||
        graph_snapshot_id_.size() != 64 ||
        node_count_ > std::uint64_t{0x100000000ULL} ||
        published_rows_ < 0)
        throw std::runtime_error("graph_node_generation_invalid");
    if (owner_lock_) {
        if (!owner_lock_->locked())
            throw std::runtime_error("native_vrs_owner_lock_required");
        if (std::filesystem::create_directories(directory_))
            std::filesystem::permissions(
                directory_, std::filesystem::perms::owner_all,
                std::filesystem::perm_options::replace);
    } else if (!std::filesystem::is_directory(directory_)) {
        throw std::runtime_error("graph_node_directory_missing");
    }
    const auto names = directory_ / "names.vrs";
    const auto reverse = directory_ / "reverse.vrs";
    if (owner_lock_ && !std::filesystem::exists(names) &&
        !std::filesystem::exists(reverse)) {
        create_data_file(names, names_magic, journal_generation_);
        create_data_file(reverse, reverse_magic, journal_generation_);
    }
    std::ifstream name_stream(names, std::ios::binary);
    std::ifstream reverse_stream(reverse, std::ios::binary);
    if (!name_stream || !reverse_stream)
        throw std::runtime_error("graph_node_data_missing");
    require_data_file(name_stream, names_magic, journal_generation_);
    require_data_file(reverse_stream, reverse_magic, journal_generation_);
    publication_ = read_publication(directory_);
    if (publication_) {
        if (publication_->journal_generation != journal_generation_ ||
            publication_->graph_snapshot_id != graph_snapshot_id_ ||
            publication_->node_count != node_count_ ||
            publication_->published_rows != published_rows_)
            throw std::runtime_error("graph_node_generation_changed");
    } else if (!owner_lock_ || node_count_ != 0 || published_rows_ != 0) {
        throw std::runtime_error("graph_node_unpublished_directory");
    }
    if (reverse_count() < node_count_)
        throw std::runtime_error("graph_node_reverse_truncated");
    const auto indexed = read_indexed(directory_, journal_generation_);
    if (indexed > reverse_count() ||
        (publication_ && indexed < publication_->node_count))
        throw std::runtime_error("graph_node_indexed_invalid");
}

// SWEGCA: src/swegca_vrs2/store.py@c06092a:242-265
std::uint64_t NativeGraphNodeDirectory::reverse_count() const {
    const auto size = std::filesystem::file_size(directory_ / "reverse.vrs");
    if (size < file_header_bytes ||
        (size - file_header_bytes) % reverse_bytes != 0)
        throw std::runtime_error("graph_node_reverse_truncated");
    const auto count = (size - file_header_bytes) / reverse_bytes;
    if (count > std::uint64_t{0x100000000ULL})
        throw std::runtime_error("graph_node_address_exhausted");
    return count;
}

// SWEGCA: src/swegca_vrs2/store.py@c06092a:242-265
std::uint64_t NativeGraphNodeDirectory::reverse_offset(
    std::uint32_t address) const {
    if (address >= reverse_count())
        throw std::runtime_error("graph_node_address_missing");
    std::ifstream stream(directory_ / "reverse.vrs", std::ios::binary);
    if (!stream) throw std::runtime_error("graph_node_reverse_truncated");
    require_data_file(stream, reverse_magic, journal_generation_);
    stream.seekg(static_cast<std::streamoff>(
        file_header_bytes + std::uint64_t(address) * reverse_bytes));
    std::array<unsigned char, reverse_bytes> entry{};
    read_exact(stream, entry.data(), entry.size());
    if (get_u32(entry.data() + 8) != address ||
        get_u32(entry.data() + 12) !=
            crc32(0, reinterpret_cast<const Bytef*>(entry.data()), 12) ||
        get_u64(entry.data()) < file_header_bytes)
        throw std::runtime_error("graph_node_reverse_corrupt");
    return get_u64(entry.data());
}

// SWEGCA: src/swegca_vrs2/store.py@c06092a:242-244
std::string NativeGraphNodeDirectory::read_name(std::uint64_t offset) const {
    const auto path = directory_ / "names.vrs";
    const auto size = std::filesystem::file_size(path);
    if (offset < file_header_bytes || offset > size || size - offset < 8)
        throw std::runtime_error("graph_node_name_truncated");
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("graph_node_name_truncated");
    require_data_file(stream, names_magic, journal_generation_);
    stream.seekg(static_cast<std::streamoff>(offset));
    std::array<unsigned char, 4> length_bytes{};
    read_exact(stream, length_bytes.data(), length_bytes.size());
    const auto length = get_u32(length_bytes.data());
    if (length == 0 || length > maximum_name_bytes ||
        length > size - offset - 8)
        throw std::runtime_error("graph_node_name_truncated");
    std::string result(length, '\0');
    read_exact(stream, reinterpret_cast<unsigned char*>(result.data()), length);
    std::array<unsigned char, 4> checksum{};
    read_exact(stream, checksum.data(), checksum.size());
    if (get_u32(checksum.data()) !=
        crc32(0, reinterpret_cast<const Bytef*>(result.data()), length))
        throw std::runtime_error("graph_node_name_corrupt");
    return result;
}

// SWEGCA: src/swegca_vrs2/store.py@c06092a:224-244
std::optional<std::pair<std::uint32_t, std::uint64_t>>
NativeGraphNodeDirectory::find_name(std::string_view name,
                                    std::uint64_t limit) const {
    const auto key = key_of(name);
    std::lock_guard guard(prefix_mutex_[key[0]]);
    if (failed_.load()) throw std::runtime_error("graph_node_directory_failed");
    for (const auto power : powers) {
        const auto path = table_path(power, key[0]);
        if (!std::filesystem::exists(path)) return std::nullopt;
        std::ifstream stream(path, std::ios::binary);
        if (!stream) throw std::runtime_error("graph_node_table_invalid");
        const auto state = read_table_header(
            stream, path, power, key[0], journal_generation_);
        const auto [offset, exists] = probe(stream, key, power);
        if (exists) {
            const auto slot = read_slot(stream, offset);
            const auto address = get_u32(slot.data() + 32);
            const auto text_offset = get_u64(slot.data() + 36);
            if (read_name(text_offset) != name)
                throw std::runtime_error("graph_node_name_digest_collision");
            if (address >= limit) return std::nullopt;
            return std::pair{address, text_offset};
        }
        if (!state.sealed) return std::nullopt;
    }
    return std::nullopt;
}

// SWEGCA: src/swegca_vrs2/store.py@c06092a:202-244
void NativeGraphNodeDirectory::require_source(
    const EventVrsInputView& source) const {
    source.require_immutable_binding();
    if (source.snapshot_id() != graph_snapshot_id_ ||
        source.node_count() != node_count_)
        throw std::runtime_error("graph_node_source_changed");
}

// SWEGCA: src/swegca_vrs2/store.py@c06092a:224-240
bool NativeGraphNodeDirectory::contains(std::string_view name) const {
    return find_name(name, node_count_).has_value();
}

// SWEGCA: src/swegca_vrs2/store.py@c06092a:232-240
std::uint32_t NativeGraphNodeDirectory::address(std::string_view name) const {
    const auto found = find_name(name, node_count_);
    if (!found) throw std::runtime_error("graph_node_name_missing");
    return found->first;
}

// SWEGCA: src/swegca_vrs2/store.py@c06092a:242-244
std::string NativeGraphNodeDirectory::name(std::uint32_t address) const {
    if (address >= node_count_)
        throw std::runtime_error("graph_node_address_missing");
    auto result = read_name(reverse_offset(address));
    (void)key_of(result);
    return result;
}

// SWEGCA: src/swegca_vrs2/store.py@c06092a:1429-1452
std::optional<GraphNodePublication>
NativeGraphNodeDirectory::publication() const {
    std::lock_guard guard(publication_mutex_);
    return publication_;
}

// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:311-390
std::filesystem::path NativeGraphNodeDirectory::insert_name(
    std::string_view name, std::uint32_t address,
    std::uint64_t name_offset) {
    const auto key = key_of(name);
    std::lock_guard guard(prefix_mutex_[key[0]]);
    if (failed_.load()) throw std::runtime_error("graph_node_directory_failed");
    try {
        for (const auto power : powers) {
            const auto path = table_path(power, key[0]);
            if (!std::filesystem::exists(path))
                create_table(path, power, key[0], journal_generation_);
            std::fstream stream(path, std::ios::in | std::ios::out |
                                       std::ios::binary);
            if (!stream) throw std::runtime_error("graph_node_table_invalid");
            auto state = read_table_header(
                stream, path, power, key[0], journal_generation_);
            const auto [offset, exists] = probe(stream, key, power);
            if (exists) {
                const auto slot = read_slot(stream, offset);
                if (read_name(get_u64(slot.data() + 36)) != name ||
                    get_u32(slot.data() + 32) != address ||
                    get_u64(slot.data() + 36) != name_offset)
                    throw std::runtime_error("graph_node_address_reassigned");
                return path;
            }
            if (state.sealed) continue;
            Slot slot{};
            std::copy(key.begin(), key.end(), slot.begin());
            put_u32(slot.data() + 32, address);
            put_u64(slot.data() + 36, name_offset);
            put_u32(slot.data() + 44,
                crc32(0, reinterpret_cast<const Bytef*>(slot.data()), 44));
            stream.seekp(static_cast<std::streamoff>(offset + key.size()));
            write_exact(stream, slot.data() + key.size(),
                        slot.size() - key.size());
            stream.seekp(static_cast<std::streamoff>(offset));
            write_exact(stream, slot.data(), key.size());
            ++state.count;
            state.sealed = state.count >=
                ((std::uint64_t{1} << power) * 7) / 10;
            std::array<unsigned char, 1> sealed{
                static_cast<unsigned char>(state.sealed)};
            stream.seekp(54);
            write_exact(stream, sealed.data(), sealed.size());
            std::array<unsigned char, 8> count{};
            put_u64(count.data(), state.count);
            stream.seekp(56);
            write_exact(stream, count.data(), count.size());
            stream.flush();
            if (!stream) throw std::runtime_error("graph_node_write_failed");
            return path;
        }
        throw std::runtime_error("graph_node_directory_capacity_exceeded");
    } catch (...) {
        failed_.store(true);
        throw;
    }
}

// SWEGCA: src/swegca_vrs2/store.py@c06092a:427-512
void NativeGraphNodeDirectory::append_committed(
    const NativeJournal& journal, const JournalAppendResult& committed,
    const MainObservationBatchPlan& batch,
    const ValidatedEventVrsInputs& parent,
    std::string_view published_parent_pair) {
    const auto& source = parent.require_validated_immutable();
    require_source(source);
    if (!batch.graph || batch.journal_rows.empty() ||
        batch.parent_pair_id != published_parent_pair ||
        committed.sequences.size() != batch.journal_rows.size() ||
        committed.frame.generation != journal_generation_ ||
        committed.frame.first_sequence != committed.sequences.front() ||
        committed.frame.last_sequence != committed.sequences.back() ||
        journal.generation() != journal_generation_ ||
        journal.row_count() <
            static_cast<std::uint64_t>(committed.frame.last_sequence) ||
        full_current_pair_snapshot_id(batch.memory_snapshot_id,
                                      batch.graph_snapshot_id) !=
            batch.pair_snapshot_id)
        throw std::runtime_error("graph_node_batch_source_changed");
    const auto& plan = *batch.graph;
    if (plan.snapshot_id != batch.graph_snapshot_id ||
        plan.parent_snapshot_id != source.snapshot_id() ||
        plan.new_nodes.empty() ||
        plan.changes.appended_direct.size() != plan.new_nodes.size() ||
        plan.changes.appended_score.size() != plan.new_nodes.size() ||
        plan.changes.appended_unresolved.size() != plan.new_nodes.size() ||
        plan.changes.appended_edges.size() !=
            plan.changes.appended_strength.size())
        throw std::runtime_error("graph_node_batch_source_changed");
    std::size_t at = 0;
    journal.visit_frame_rows(committed.frame, [&](JournalRow&& actual) {
        if (at >= batch.journal_rows.size() ||
            actual.sequence != committed.sequences[at] ||
            actual.request_id != batch.journal_rows[at].request_id ||
            actual.body != batch.journal_rows[at].body ||
            actual.fingerprint != batch.journal_rows[at].fingerprint ||
            actual.pair_id != batch.journal_rows[at].pair_id ||
            actual.pair_id != batch.pair_snapshot_id ||
            parse_native_journal_entry(actual.request_id, actual.body,
                                       actual.fingerprint).kind !=
                NativeJournalEntryKind::observation)
            throw std::runtime_error("graph_node_batch_source_changed");
        ++at;
    });
    if (at != batch.journal_rows.size())
        throw std::runtime_error("graph_node_batch_source_changed");
    append(plan.new_nodes);
    // The writer follows each unpublished source generation during recovery;
    // existing readers retain their earlier immutable binding and row limit.
    // SWEGCA: src/swegca_vrs2/store.py@c06092a:1429-1452
    graph_snapshot_id_ = plan.snapshot_id;
    node_count_ = source.node_count() + plan.new_nodes.size();
}

// SWEGCA: src/swegca_vrs2/store.py@c06092a:427-472
// SWEGCA: user@2026-09-22:89-92
void NativeGraphNodeDirectory::append(
    std::span<const std::pair<std::string, std::uint32_t>> nodes) {
    if (!owner_lock_ || !owner_lock_->locked())
        throw std::runtime_error("native_vrs_owner_lock_required");
    if (failed_.load()) throw std::runtime_error("graph_node_directory_failed");
    if (nodes.empty()) return;
    std::lock_guard append_guard(append_mutex_);
    if (nodes.size() > std::uint64_t{0x100000000ULL} - node_count_)
        throw std::runtime_error("graph_node_address_exhausted");
    std::set<std::string> seen;
    for (std::size_t at = 0; at < nodes.size(); ++at) {
        const auto expected_address = node_count_ + at;
        if (nodes[at].second != expected_address ||
            nodes[at].first.empty() ||
            nodes[at].first.size() > maximum_name_bytes ||
            !seen.insert(nodes[at].first).second)
            throw std::runtime_error("graph_node_append_order_changed");
        const auto old = find_name(nodes[at].first,
                                   std::uint64_t{0x100000000ULL});
        if (old && old->first != nodes[at].second)
            throw std::runtime_error("graph_node_address_reassigned");
    }
    const auto already = reverse_count();
    if (already < node_count_ || already > node_count_ + nodes.size())
        throw std::runtime_error("graph_node_reverse_generation_changed");
    const auto names_path = directory_ / "names.vrs";
    const auto reverse_path = directory_ / "reverse.vrs";
    std::ofstream names_stream(names_path, std::ios::binary | std::ios::app);
    std::ofstream reverse_stream(reverse_path, std::ios::binary | std::ios::app);
    if (!names_stream || !reverse_stream)
        throw std::runtime_error("graph_node_write_failed");
    names_stream.seekp(0, std::ios::end);
    reverse_stream.seekp(0, std::ios::end);
    if (!names_stream || !reverse_stream)
        throw std::runtime_error("graph_node_write_failed");
    std::vector<std::uint64_t> offsets;
    offsets.reserve(nodes.size());
    try {
        for (std::size_t at = 0; at < nodes.size(); ++at) {
            const auto address = nodes[at].second;
            if (address < already) {
                const auto offset = reverse_offset(address);
                if (read_name(offset) != nodes[at].first)
                    throw std::runtime_error("graph_node_address_reassigned");
                offsets.push_back(offset);
                continue;
            }
            const auto position = names_stream.tellp();
            if (position == std::streampos(-1))
                throw std::runtime_error("graph_node_write_failed");
            const auto offset = static_cast<std::uint64_t>(position);
            std::array<unsigned char, 4> length{};
            put_u32(length.data(),
                    static_cast<std::uint32_t>(nodes[at].first.size()));
            write_exact(names_stream, length.data(), length.size());
            names_stream.write(nodes[at].first.data(),
                static_cast<std::streamsize>(nodes[at].first.size()));
            const auto checksum = crc32(0,
                reinterpret_cast<const Bytef*>(nodes[at].first.data()),
                static_cast<uInt>(nodes[at].first.size()));
            std::array<unsigned char, 4> crc{};
            put_u32(crc.data(), checksum);
            write_exact(names_stream, crc.data(), crc.size());
            std::array<unsigned char, reverse_bytes> reverse{};
            put_u64(reverse.data(), offset);
            put_u32(reverse.data() + 8, address);
            put_u32(reverse.data() + 12,
                crc32(0, reinterpret_cast<const Bytef*>(reverse.data()), 12));
            write_exact(reverse_stream, reverse.data(), reverse.size());
            offsets.push_back(offset);
        }
        names_stream.flush();
        reverse_stream.flush();
        if (!names_stream || !reverse_stream)
            throw std::runtime_error("graph_node_write_failed");
        names_stream.close();
        reverse_stream.close();
        if (!names_stream || !reverse_stream)
            throw std::runtime_error("graph_node_write_failed");
        sync_file(names_path);
        sync_file(reverse_path);
        std::array<std::vector<std::size_t>, 16> groups;
        for (std::size_t at = 0; at < nodes.size(); ++at)
            groups[key_of(nodes[at].first)[0] >> 4].push_back(at);
        std::mutex error_mutex;
        std::exception_ptr worker_error;
        std::vector<std::jthread> workers;
        workers.reserve(groups.size());
        for (std::size_t group = 0; group < groups.size(); ++group) {
            workers.emplace_back([&, group] {
                try {
                    std::set<std::filesystem::path> touched;
                    for (const auto at : groups[group])
                        touched.insert(insert_name(
                            nodes[at].first, nodes[at].second,
                            offsets[at]));
                    for (const auto& path : touched) sync_file(path);
                } catch (...) {
                    std::lock_guard guard(error_mutex);
                    if (!worker_error)
                        worker_error = std::current_exception();
                }
            });
        }
        workers.clear();
        if (worker_error) std::rethrow_exception(worker_error);
        if (reverse_count() != node_count_ + nodes.size())
            throw std::runtime_error("graph_node_reverse_generation_changed");
        write_indexed(directory_, journal_generation_, reverse_count());
    } catch (...) {
        failed_.store(true);
        throw;
    }
}

// SWEGCA: src/swegca_vrs2/store.py@c06092a:1429-1452
void NativeGraphNodeDirectory::publish(
    const NativeJournal& journal, const ValidatedEventVrsInputs& successor,
    std::string_view memory_snapshot_id, std::string_view pair_snapshot_id,
    std::int64_t journal_rows) {
    if (!owner_lock_ || !owner_lock_->locked())
        throw std::runtime_error("native_vrs_owner_lock_required");
    const auto& source = successor.require_validated_immutable();
    std::lock_guard append_guard(append_mutex_);
    const auto count = reverse_count();
    const auto head = journal.head();
    const bool matching_journal = journal.generation() == journal_generation_ &&
        ((journal_rows == 0 && !head && journal.row_count() == 0) ||
         (head && head->first == journal_rows &&
          head->second == pair_snapshot_id));
    if (failed_.load() || count != source.node_count() ||
        read_indexed(directory_, journal_generation_) != count ||
        memory_snapshot_id.empty() ||
        full_current_pair_snapshot_id(memory_snapshot_id,
                                      source.snapshot_id()) != pair_snapshot_id ||
        !matching_journal ||
        journal_rows < published_rows_)
        throw std::runtime_error("graph_node_publication_invalid");
    std::vector<std::unique_lock<std::mutex>> prefix_guards;
    prefix_guards.reserve(prefix_mutex_.size());
    for (auto& mutex : prefix_mutex_) prefix_guards.emplace_back(mutex);
    std::lock_guard published(publication_mutex_);
    if (publication_ && journal_rows < publication_->published_rows)
        throw std::runtime_error("graph_node_publication_regressed");
    try {
        for (const auto& entry : std::filesystem::directory_iterator(directory_))
            if (entry.is_regular_file() &&
                entry.path().filename() !=
                    std::filesystem::path(std::string(publication_name)))
                sync_file(entry.path());
        Json::Object body;
        body.emplace("schema", Json(std::string(publication_schema)));
        body.emplace("journal_generation", Json(journal_generation_));
        body.emplace("graph_snapshot_id", Json(source.snapshot_id()));
        body.emplace("pair_snapshot_id", Json(std::string(pair_snapshot_id)));
        body.emplace("published_rows", Json(journal_rows));
        body.emplace("node_count", Json(static_cast<std::int64_t>(count)));
        const auto bytes = Json(std::move(body)).canonical();
        write_atomic_file(directory_ / publication_name,
                          std::as_bytes(std::span(bytes)));
        publication_ = GraphNodePublication{
            journal_generation_, source.snapshot_id(),
            std::string(pair_snapshot_id), journal_rows, count};
        graph_snapshot_id_ = publication_->graph_snapshot_id;
        node_count_ = count;
        published_rows_ = journal_rows;
    } catch (...) {
        failed_.store(true);
        throw;
    }
}

}  // namespace swegca::vrs
