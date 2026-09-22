#include "native_cue_directory.hpp"

#include "digest.hpp"
#include "journal_files.hpp"
#include "json.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <memory>
#include <queue>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>
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

constexpr std::string_view table_magic = "VRSCUT04";
constexpr std::string_view cue_magic = "VRSCUN04";
constexpr std::string_view posting_magic = "VRSCUP04";
constexpr std::string_view publication_schema = "swegca-vrs2-native-cue-directory-v1";
constexpr std::string_view publication_name = "PUBLISHED.json";
constexpr std::uint64_t table_header_bytes = 4096;
constexpr std::uint64_t data_header_bytes = 64;
constexpr std::uint64_t slot_bytes = 96;
constexpr std::uint64_t posting_bytes = 60;
constexpr std::uint64_t state_offset = 80;
constexpr std::array<unsigned, 7> powers{12, 14, 16, 18, 20, 22, 23};
constexpr std::uint32_t maximum_cue_bytes = 1024 * 1024;
using Key = std::array<unsigned char, 32>;

struct LevelState {
    std::uint64_t count;
    bool sealed;
};

struct HeadCopy {
    std::uint64_t offset;
    std::uint64_t count;
    std::uint64_t sequence;
    unsigned index;
};

// SWEGCA: src/swegca_vrs2/cue_shards.py@c06092a:130-158
std::uint64_t get_u64(const unsigned char* bytes) {
    std::uint64_t value = 0;
    for (unsigned at = 0; at < 8; ++at)
        value |= std::uint64_t(bytes[at]) << (8 * at);
    return value;
}

// SWEGCA: src/swegca_vrs2/cue_shards.py@c06092a:130-158
void put_u64(unsigned char* bytes, std::uint64_t value) {
    for (unsigned at = 0; at < 8; ++at)
        bytes[at] = static_cast<unsigned char>((value >> (8 * at)) & 0xff);
}

// SWEGCA: src/swegca_vrs2/cue_shards.py@c06092a:130-158
std::uint32_t get_u32(const unsigned char* bytes) {
    std::uint32_t value = 0;
    for (unsigned at = 0; at < 4; ++at)
        value |= std::uint32_t(bytes[at]) << (8 * at);
    return value;
}

// SWEGCA: src/swegca_vrs2/cue_shards.py@c06092a:130-158
void put_u32(unsigned char* bytes, std::uint32_t value) {
    for (unsigned at = 0; at < 4; ++at)
        bytes[at] = static_cast<unsigned char>((value >> (8 * at)) & 0xff);
}

// SWEGCA: src/swegca_vrs2/cue_shards.py@c06092a:75-91
std::array<std::byte, data_header_bytes> data_header(
    std::string_view magic, std::string_view generation) {
    if (magic.size() != 8 || generation.size() != 34 ||
        !generation.starts_with("g-"))
        throw std::runtime_error("native_cue_generation_invalid");
    std::array<std::byte, data_header_bytes> header{};
    for (std::size_t at = 0; at < magic.size(); ++at)
        header[at] = static_cast<std::byte>(magic[at]);
    for (std::size_t at = 0; at < generation.size(); ++at)
        header[16 + at] = static_cast<std::byte>(generation[at]);
    return header;
}

// SWEGCA: src/swegca_vrs2/cue_shards.py@c06092a:75-91
void require_data_header(std::istream& stream, std::string_view magic,
                         std::string_view generation) {
    std::array<std::byte, data_header_bytes> found{};
    stream.seekg(0);
    stream.read(reinterpret_cast<char*>(found.data()), found.size());
    if (!stream || found != data_header(magic, generation))
        throw std::runtime_error("native_cue_data_header_invalid");
}

// SWEGCA: src/swegca_vrs2/cue_shards.py@c06092a:75-91
void sync_file(const std::filesystem::path& path) {
#if defined(_WIN32)
    const auto descriptor = _wopen(path.c_str(), _O_BINARY | _O_RDONLY);
    if (descriptor < 0) throw std::runtime_error("native_cue_sync_failed");
    const auto result = _commit(descriptor);
    _close(descriptor);
#else
    const auto descriptor = open(path.c_str(), O_RDONLY);
    if (descriptor < 0) throw std::runtime_error("native_cue_sync_failed");
    const auto result = fsync(descriptor);
    close(descriptor);
#endif
    if (result != 0) throw std::runtime_error("native_cue_sync_failed");
}

// SWEGCA: src/swegca_vrs2/cue_shards.py@c06092a:96-123
std::uint64_t table_size(unsigned power) {
    return table_header_bytes + (std::uint64_t{1} << power) * slot_bytes;
}

// SWEGCA: src/swegca_vrs2/cue_shards.py@c06092a:96-123
void create_table(const std::filesystem::path& path, unsigned power,
                  std::uint8_t prefix, std::string_view generation) {
    std::array<unsigned char, table_header_bytes> header{};
    std::copy(table_magic.begin(), table_magic.end(), header.begin());
    header[16] = prefix;
    header[17] = static_cast<unsigned char>(power);
    std::copy(generation.begin(), generation.end(), header.begin() + 32);
    write_atomic_file(path, std::as_bytes(std::span(header)));
    std::filesystem::resize_file(path, table_size(power));
    sync_file(path);
}

// SWEGCA: src/swegca_vrs2/cue_shards.py@c06092a:159-191
LevelState read_table_header(std::istream& stream,
                             const std::filesystem::path& path, unsigned power,
                             std::uint8_t prefix, std::string_view generation) {
    if (std::filesystem::file_size(path) != table_size(power))
        throw std::runtime_error("native_cue_table_invalid");
    std::array<unsigned char, 96> header{};
    stream.seekg(0);
    stream.read(reinterpret_cast<char*>(header.data()), header.size());
    if (!stream || !std::equal(table_magic.begin(), table_magic.end(), header.begin()) ||
        header[16] != prefix || header[17] != power ||
        !std::equal(generation.begin(), generation.end(), header.begin() + 32))
        throw std::runtime_error("native_cue_table_invalid");
    const auto count = get_u64(header.data() + state_offset);
    const auto sealed = header[state_offset + 8];
    if (count > (std::uint64_t{1} << power) || sealed > 1)
        throw std::runtime_error("native_cue_table_invalid");
    return LevelState{count, sealed != 0};
}

// SWEGCA: src/swegca_vrs2/cue_shards.py@c06092a:171-191
void write_table_state(std::ostream& stream, LevelState state) {
    std::array<unsigned char, 9> bytes{};
    put_u64(bytes.data(), state.count);
    bytes[8] = static_cast<unsigned char>(state.sealed);
    stream.seekp(static_cast<std::streamoff>(state_offset));
    stream.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (!stream) throw std::runtime_error("native_cue_table_write_failed");
}

// SWEGCA: src/swegca_vrs2/cue_shards.py@c06092a:143-158
std::pair<std::uint64_t, bool> probe(std::istream& stream, const Key& key,
                                     unsigned power) {
    const auto mask = (std::uint64_t{1} << power) - 1;
    const auto start = get_u64(key.data() + 1) & mask;
    const auto step = (get_u64(key.data() + 9) | 1) & mask;
    for (std::uint64_t count = 0; count <= mask; ++count) {
        const auto position = (start + count * step) & mask;
        const auto offset = table_header_bytes + position * slot_bytes;
        std::array<unsigned char, slot_bytes> slot{};
        stream.clear();
        stream.seekg(static_cast<std::streamoff>(offset));
        stream.read(reinterpret_cast<char*>(slot.data()), slot.size());
        if (!stream) throw std::runtime_error("native_cue_table_invalid");
        if (std::equal(key.begin(), key.end(), slot.begin()))
            return {offset, true};
        if (std::all_of(slot.begin(), slot.begin() + 32,
                        [](auto byte) { return byte == 0; })) {
            if (!std::all_of(slot.begin() + 32, slot.end(),
                             [](auto byte) { return byte == 0; }))
                throw std::runtime_error("native_cue_slot_corrupt");
            return {offset, false};
        }
    }
    throw std::runtime_error("native_cue_segment_full");
}

// SWEGCA: src/swegca_vrs2/cue_shards.py@c06092a:202-221
std::optional<HeadCopy> valid_copy(const unsigned char* bytes, unsigned index) {
    const auto head = get_u64(bytes);
    const auto count = get_u64(bytes + 8);
    const auto sequence = get_u64(bytes + 16);
    const auto expected = get_u32(bytes + 24);
    if (!head && !count && !sequence && !expected) return std::nullopt;
    const auto actual = crc32(0, reinterpret_cast<const Bytef*>(bytes), 24);
    if (actual != expected || head < data_header_bytes || !count || !sequence ||
        sequence > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        return std::nullopt;
    return HeadCopy{head, count, sequence, index};
}

// SWEGCA: src/swegca_vrs2/cue_shards.py@c06092a:202-221
HeadCopy read_head_copy(const std::array<unsigned char, slot_bytes>& slot) {
    const auto first = valid_copy(slot.data() + 40, 0);
    const auto second = valid_copy(slot.data() + 68, 1);
    if (!first && !second) throw std::runtime_error("native_cue_head_invalid");
    if (!first) return *second;
    if (!second) return *first;
    if (first->sequence == second->sequence &&
        (first->offset != second->offset || first->count != second->count))
        throw std::runtime_error("native_cue_head_invalid");
    return first->sequence >= second->sequence ? *first : *second;
}

// SWEGCA: src/swegca_vrs2/cue_shards.py@c06092a:193-221
std::array<unsigned char, 28> encode_copy(std::uint64_t head,
                                           std::uint64_t count,
                                           std::uint64_t sequence) {
    std::array<unsigned char, 28> bytes{};
    put_u64(bytes.data(), head);
    put_u64(bytes.data() + 8, count);
    put_u64(bytes.data() + 16, sequence);
    put_u32(bytes.data() + 24,
            crc32(0, reinterpret_cast<const Bytef*>(bytes.data()), 24));
    return bytes;
}

// SWEGCA: src/swegca_vrs2/cue_shards.py@c06092a:223-244
std::string read_cue(const std::filesystem::path& path, std::uint64_t offset,
                     std::string_view generation) {
    const auto size = std::filesystem::file_size(path);
    if (offset < data_header_bytes || offset > size || size - offset < 8)
        throw std::runtime_error("native_cue_text_corrupt");
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("native_cue_text_corrupt");
    require_data_header(stream, cue_magic, generation);
    std::array<unsigned char, 8> header{};
    stream.seekg(static_cast<std::streamoff>(offset));
    stream.read(reinterpret_cast<char*>(header.data()), header.size());
    if (!stream) throw std::runtime_error("native_cue_text_corrupt");
    const auto length = get_u32(header.data());
    if (!length || length > maximum_cue_bytes || length > size - offset - 8)
        throw std::runtime_error("native_cue_text_corrupt");
    std::string cue(length, '\0');
    stream.read(cue.data(), cue.size());
    if (!stream || crc32(0, reinterpret_cast<const Bytef*>(cue.data()),
                         cue.size()) != get_u32(header.data() + 4))
        throw std::runtime_error("native_cue_text_corrupt");
    return cue;
}

// SWEGCA: src/swegca_vrs2/cue_shards.py@c06092a:223-244
std::uint64_t append_cue(const std::filesystem::path& path,
                         std::string_view cue, std::string_view generation) {
    if (cue.empty() || cue.size() > maximum_cue_bytes)
        throw std::runtime_error("invalid_vrs_cue");
    const auto offset = std::filesystem::file_size(path);
    if (offset < data_header_bytes ||
        offset > std::numeric_limits<std::uint64_t>::max() - 8 - cue.size())
        throw std::runtime_error("native_cue_text_corrupt");
    std::ifstream check(path, std::ios::binary);
    if (!check) throw std::runtime_error("native_cue_text_corrupt");
    require_data_header(check, cue_magic, generation);
    check.close();
    std::array<unsigned char, 8> header{};
    put_u32(header.data(), static_cast<std::uint32_t>(cue.size()));
    put_u32(header.data() + 4,
            crc32(0, reinterpret_cast<const Bytef*>(cue.data()), cue.size()));
    std::ofstream stream(path, std::ios::binary | std::ios::app);
    if (!stream) throw std::runtime_error("native_cue_write_failed");
    stream.write(reinterpret_cast<const char*>(header.data()), header.size());
    stream.write(cue.data(), static_cast<std::streamsize>(cue.size()));
    stream.flush();
    if (!stream) throw std::runtime_error("native_cue_write_failed");
    stream.close();
    sync_file(path);
    return offset;
}

struct RawPosting {
    std::uint64_t previous;
    std::int64_t sequence;
    std::uint64_t cumulative_count;
    std::string episode_id;
};

// SWEGCA: src/swegca_vrs2/cue_shards.py@c06092a:246-258
std::array<unsigned char, posting_bytes> encode_node(
    std::uint64_t previous, std::int64_t sequence,
    std::uint64_t count, std::string_view episode_id) {
    if (sequence < 1 || !count || episode_id.size() != 71 ||
        !episode_id.starts_with("memory:"))
        throw std::runtime_error("native_cue_posting_invalid");
    std::array<unsigned char, posting_bytes> bytes{};
    put_u64(bytes.data(), previous);
    put_u64(bytes.data() + 8, static_cast<std::uint64_t>(sequence));
    put_u64(bytes.data() + 16, count);
    for (std::size_t at = 0; at < 32; ++at) {
        const auto digit = [](char value) -> int {
            if (value >= '0' && value <= '9') return value - '0';
            if (value >= 'a' && value <= 'f') return value - 'a' + 10;
            return -1;
        };
        const auto high = digit(episode_id[7 + at * 2]);
        const auto low = digit(episode_id[8 + at * 2]);
        if (high < 0 || low < 0)
            throw std::runtime_error("native_cue_posting_invalid");
        bytes[24 + at] = static_cast<unsigned char>((high << 4) | low);
    }
    put_u32(bytes.data() + 56,
            crc32(0, reinterpret_cast<const Bytef*>(bytes.data()), 56));
    return bytes;
}

// SWEGCA: src/swegca_vrs2/cue_shards.py@c06092a:246-258
RawPosting decode_node(const std::array<unsigned char, posting_bytes>& bytes) {
    if (get_u32(bytes.data() + 56) !=
        crc32(0, reinterpret_cast<const Bytef*>(bytes.data()), 56))
        throw std::runtime_error("native_cue_posting_corrupt");
    const auto previous = get_u64(bytes.data());
    const auto sequence = get_u64(bytes.data() + 8);
    const auto count = get_u64(bytes.data() + 16);
    if (sequence < 1 || sequence > static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max()) || !count ||
        (count == 1 && previous != 0) || (count > 1 && previous < data_header_bytes))
        throw std::runtime_error("native_cue_posting_corrupt");
    constexpr char hex[] = "0123456789abcdef";
    std::string identifier = "memory:";
    identifier.reserve(71);
    for (std::size_t at = 24; at < 56; ++at) {
        identifier.push_back(hex[bytes[at] >> 4]);
        identifier.push_back(hex[bytes[at] & 15]);
    }
    return RawPosting{previous, static_cast<std::int64_t>(sequence),
                      count, std::move(identifier)};
}

// SWEGCA: src/swegca_vrs2/cue_shards.py@c06092a:246-258
RawPosting read_node_from_stream(std::istream& stream, std::uint64_t offset,
                                 std::uint64_t size) {
    if (offset < data_header_bytes || offset > size ||
        posting_bytes > size - offset)
        throw std::runtime_error("native_cue_posting_truncated");
    std::array<unsigned char, posting_bytes> bytes{};
    stream.clear();
    stream.seekg(static_cast<std::streamoff>(offset));
    stream.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
    if (!stream) throw std::runtime_error("native_cue_posting_truncated");
    return decode_node(bytes);
}

// SWEGCA: src/swegca_vrs2/cue_shards.py@c06092a:246-258
RawPosting read_node(const std::filesystem::path& path, std::uint64_t offset,
                     std::string_view generation) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("native_cue_posting_truncated");
    require_data_header(stream, posting_magic, generation);
    return read_node_from_stream(stream, offset, std::filesystem::file_size(path));
}

// SWEGCA: src/swegca_vrs2/cue_shards.py@c06092a:246-258
std::uint64_t append_node(const std::filesystem::path& path,
                          std::uint64_t previous, std::int64_t sequence,
                          std::uint64_t count, std::string_view episode_id,
                          std::string_view generation) {
    const auto offset = std::filesystem::file_size(path);
    if (offset < data_header_bytes ||
        offset > std::numeric_limits<std::uint64_t>::max() - posting_bytes)
        throw std::runtime_error("native_cue_posting_invalid");
    std::ifstream check(path, std::ios::binary);
    if (!check) throw std::runtime_error("native_cue_posting_invalid");
    require_data_header(check, posting_magic, generation);
    check.close();
    const auto bytes = encode_node(previous, sequence, count, episode_id);
    std::ofstream stream(path, std::ios::binary | std::ios::app);
    if (!stream) throw std::runtime_error("native_cue_posting_write_failed");
    stream.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    stream.flush();
    if (!stream) throw std::runtime_error("native_cue_posting_write_failed");
    stream.close();
    sync_file(path);
    return offset;
}

// SWEGCA: src/swegca_vrs2/cue_shards.py@c06092a:223-244
void create_data_file(const std::filesystem::path& path,
                      std::string_view magic, std::string_view generation) {
    const auto header = data_header(magic, generation);
    write_atomic_file(path, std::span<const std::byte>(header));
}

// SWEGCA: src/swegca_vrs2/cue_shards.py@c06092a:193-221
std::array<unsigned char, slot_bytes> read_slot(std::istream& stream,
                                                 std::uint64_t offset) {
    std::array<unsigned char, slot_bytes> slot{};
    stream.clear();
    stream.seekg(static_cast<std::streamoff>(offset));
    stream.read(reinterpret_cast<char*>(slot.data()), slot.size());
    if (!stream) throw std::runtime_error("native_cue_table_invalid");
    return slot;
}

// SWEGCA: src/swegca_vrs2/cue_shards.py@c06092a:1-22
std::optional<CueDirectoryPublication> read_publication(
    const std::filesystem::path& directory) {
    const auto path = directory / publication_name;
    if (!std::filesystem::exists(path)) return std::nullopt;
    if (std::filesystem::file_size(path) > 4096)
        throw std::runtime_error("native_cue_publication_invalid");
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("native_cue_publication_invalid");
    const std::string content(std::istreambuf_iterator<char>{stream}, {});
    if (stream.bad()) throw std::runtime_error("native_cue_publication_invalid");
    try {
        const auto value = Json::parse(content);
        if (value.object().size() != 4 ||
            value.at("schema").string() != publication_schema)
            throw std::runtime_error("native_cue_publication_invalid");
        auto generation = value.at("journal_generation").string();
        const auto rows = value.at("published_rows").integer();
        auto pair = value.at("pair_snapshot_id").string();
        if (generation.size() != 34 || !generation.starts_with("g-") ||
            rows < 0 || pair.empty())
            throw std::runtime_error("native_cue_publication_invalid");
        return CueDirectoryPublication{std::move(generation), rows,
                                       std::move(pair)};
    } catch (...) {
        throw std::runtime_error("native_cue_publication_invalid");
    }
}

}  // namespace

// SWEGCA: src/swegca_vrs2/cue_shards.py@c06092a:42-48
NativeCueDirectory::Key NativeCueDirectory::key_of(std::string_view cue) {
    if (cue.empty() || cue.size() > maximum_cue_bytes)
        throw std::runtime_error("invalid_vrs_cue");
    const auto hex = sha256_hex(cue);
    Key key{};
    const auto nibble = [](char value) -> unsigned char {
        if (value >= '0' && value <= '9') return value - '0';
        return value - 'a' + 10;
    };
    for (std::size_t at = 0; at < key.size(); ++at)
        key[at] = static_cast<unsigned char>(
            (nibble(hex[at * 2]) << 4) | nibble(hex[at * 2 + 1]));
    if (std::all_of(key.begin(), key.end(), [](auto value) { return value == 0; }))
        throw std::runtime_error("reserved_vrs_cue_digest");
    return key;
}

// SWEGCA: src/swegca_vrs2/cue_shards.py@c06092a:92-123
NativeCueDirectory::NativeCueDirectory(
    std::filesystem::path directory, std::string journal_generation,
    OwnerLock* owner_lock)
    : directory_(std::move(directory)),
      journal_generation_(std::move(journal_generation)),
      owner_lock_(owner_lock) {
    (void)data_header(cue_magic, journal_generation_);
    if (owner_lock_) {
        if (!owner_lock_->locked())
            throw std::runtime_error("native_vrs_owner_lock_required");
        if (std::filesystem::create_directories(directory_))
            std::filesystem::permissions(directory_,
                std::filesystem::perms::owner_all,
                std::filesystem::perm_options::replace);
        if (!std::filesystem::is_empty(directory_))
            throw std::runtime_error("native_cue_rebuild_required");
    } else if (!std::filesystem::is_directory(directory_)) {
        throw std::runtime_error("native_cue_directory_missing");
    }
    publication_ = read_publication(directory_);
    if (publication_) {
        if (publication_->journal_generation != journal_generation_)
            throw std::runtime_error("native_cue_generation_changed");
    } else if (!owner_lock_) {
        throw std::runtime_error("native_cue_unpublished_directory");
    }
}

// SWEGCA: src/swegca_vrs2/cue_shards.py@c06092a:92-123
std::filesystem::path NativeCueDirectory::table_path(
    unsigned power, std::uint8_t prefix) const {
    std::ostringstream name;
    name << "cue-p" << power << '-' << std::hex << std::setw(2)
         << std::setfill('0') << static_cast<unsigned>(prefix) << ".vrs";
    return directory_ / name.str();
}

// SWEGCA: src/swegca_vrs2/cue_shards.py@c06092a:75-91
std::filesystem::path NativeCueDirectory::cue_path(std::uint8_t prefix) const {
    std::ostringstream name;
    name << "cue-text-" << std::hex << std::setw(2) << std::setfill('0')
         << static_cast<unsigned>(prefix) << ".vrs";
    return directory_ / name.str();
}

// SWEGCA: src/swegca_vrs2/cue_shards.py@c06092a:75-91
std::filesystem::path NativeCueDirectory::posting_path(std::uint8_t prefix) const {
    std::ostringstream name;
    name << "cue-posting-" << std::hex << std::setw(2) << std::setfill('0')
         << static_cast<unsigned>(prefix) << ".vrs";
    return directory_ / name.str();
}

// SWEGCA: src/swegca_vrs2/cue_shards.py@c06092a:246-258
NativeCueDirectory::PostingNode NativeCueDirectory::node_at(
    std::uint8_t prefix, std::uint64_t offset) const {
    const auto raw = read_node(posting_path(prefix), offset, journal_generation_);
    if (raw.previous && raw.previous >= offset)
        throw std::runtime_error("native_cue_posting_corrupt");
    return PostingNode{raw.previous, raw.sequence, raw.cumulative_count,
                       raw.episode_id};
}

// SWEGCA: src/swegca_vrs2/cue_shards.py@c06092a:260-360
// SWEGCA: src/swegca_vrs2/store.py@7536139:154-167
void NativeCueDirectory::put(std::span<const std::string> cues,
                             std::string_view episode_id,
                             std::int64_t journal_sequence) {
    if (!owner_lock_ || !owner_lock_->locked())
        throw std::runtime_error("native_vrs_owner_lock_required");
    if (failed_.load()) throw std::runtime_error("native_cue_rebuild_required");
    (void)encode_node(0, journal_sequence, 1, episode_id);
    std::set<std::string> distinct(cues.begin(), cues.end());
    for (const auto& cue : distinct) {
        const auto key = key_of(cue);
        const auto prefix = key[0];
        std::lock_guard guard(prefix_mutex_[prefix]);
        try {
            const auto text_path = cue_path(prefix);
            const auto nodes_path = posting_path(prefix);
            if (!std::filesystem::exists(text_path))
                create_data_file(text_path, cue_magic, journal_generation_);
            if (!std::filesystem::exists(nodes_path))
                create_data_file(nodes_path, posting_magic, journal_generation_);
            bool inserted = false;
            for (const auto power : powers) {
                const auto path = table_path(power, prefix);
                if (!std::filesystem::exists(path))
                    create_table(path, power, prefix, journal_generation_);
                std::fstream table(path, std::ios::in | std::ios::out | std::ios::binary);
                if (!table) throw std::runtime_error("native_cue_table_invalid");
                auto state = read_table_header(table, path, power, prefix,
                                               journal_generation_);
                const auto [slot_offset, exists] = probe(table, key, power);
                if (!exists && state.sealed) continue;
                std::uint64_t previous = 0;
                std::uint64_t count = 0;
                unsigned target_copy = 0;
                std::uint64_t cue_offset = 0;
                if (exists) {
                    const auto slot = read_slot(table, slot_offset);
                    cue_offset = get_u64(slot.data() + 32);
                    if (read_cue(text_path, cue_offset, journal_generation_) != cue)
                        throw std::runtime_error("native_cue_digest_collision");
                    const auto head = read_head_copy(slot);
                    previous = head.offset;
                    count = head.count;
                    target_copy = 1 - head.index;
                    if (head.sequence >= static_cast<std::uint64_t>(journal_sequence)) {
                        const auto current = node_at(prefix, head.offset);
                        if (current.sequence == journal_sequence &&
                            current.episode_id == episode_id) {
                            inserted = true;
                            break;
                        }
                        throw std::runtime_error("native_cue_posting_order_changed");
                    }
                } else {
                    cue_offset = append_cue(text_path, cue, journal_generation_);
                }
                const auto head_offset = append_node(nodes_path, previous,
                    journal_sequence, count + 1, episode_id, journal_generation_);
                const auto copy = encode_copy(head_offset, count + 1,
                    static_cast<std::uint64_t>(journal_sequence));
                table.clear();
                if (!exists) {
                    std::array<unsigned char, 8> cue_bytes{};
                    put_u64(cue_bytes.data(), cue_offset);
                    table.seekp(static_cast<std::streamoff>(slot_offset + 32));
                    table.write(reinterpret_cast<const char*>(cue_bytes.data()),
                                cue_bytes.size());
                    table.seekp(static_cast<std::streamoff>(slot_offset + 40));
                    table.write(reinterpret_cast<const char*>(copy.data()), copy.size());
                    table.seekp(static_cast<std::streamoff>(slot_offset));
                    table.write(reinterpret_cast<const char*>(key.data()), key.size());
                    ++state.count;
                    state.sealed = state.count >=
                        ((std::uint64_t{1} << power) * 7) / 10;
                    write_table_state(table, state);
                } else {
                    table.seekp(static_cast<std::streamoff>(
                        slot_offset + 40 + 28 * target_copy));
                    table.write(reinterpret_cast<const char*>(copy.data()),
                                copy.size());
                }
                table.flush();
                if (!table) throw std::runtime_error("native_cue_table_write_failed");
                table.close();
                sync_file(path);
                inserted = true;
                break;
            }
            if (!inserted)
                throw std::runtime_error("native_cue_directory_capacity_exceeded");
        } catch (...) {
            failed_.store(true);
            throw;
        }
    }
}

// SWEGCA: src/swegca_vrs2/cue_shards.py@c06092a:392-440
// SWEGCA: src/swegca_vrs2/engine/mosaic_memory_activation.py@7536139:246-267
std::optional<NativeCueDirectory::CueHead> NativeCueDirectory::head_for(
    std::string_view cue, std::int64_t published_row_limit) const {
    const auto key = key_of(cue);
    const auto prefix = key[0];
    std::lock_guard guard(prefix_mutex_[prefix]);
    if (failed_.load() || published_row_limit < 0)
        throw std::runtime_error("native_cue_read_generation_invalid");
    if (!owner_lock_) {
        std::lock_guard published(publication_mutex_);
        if (!publication_ || published_row_limit > publication_->published_rows)
            throw std::runtime_error("native_cue_unpublished_read");
    }
    for (const auto power : powers) {
        const auto path = table_path(power, prefix);
        if (!std::filesystem::exists(path)) return std::nullopt;
        std::ifstream table(path, std::ios::binary);
        if (!table) throw std::runtime_error("native_cue_table_invalid");
        const auto state = read_table_header(table, path, power, prefix,
                                             journal_generation_);
        const auto [slot_offset, exists] = probe(table, key, power);
        if (exists) {
            const auto slot = read_slot(table, slot_offset);
            const auto offset = get_u64(slot.data() + 32);
            if (read_cue(cue_path(prefix), offset, journal_generation_) != cue)
                throw std::runtime_error("native_cue_digest_collision");
            const auto copy = read_head_copy(slot);
            auto head = CueHead{prefix, copy.offset, copy.count,
                static_cast<std::int64_t>(copy.sequence)};
            while (head.sequence > published_row_limit) {
                const auto current = node_at(prefix, head.head);
                if (current.sequence != head.sequence ||
                    current.cumulative_count != head.count)
                    throw std::runtime_error("native_cue_posting_count_invalid");
                if (!current.previous) return std::nullopt;
                const auto prior = node_at(prefix, current.previous);
                if (prior.sequence >= current.sequence ||
                    prior.cumulative_count + 1 != current.cumulative_count)
                    throw std::runtime_error("native_cue_posting_order_changed");
                head = CueHead{prefix, current.previous,
                               prior.cumulative_count, prior.sequence};
            }
            return head;
        }
        if (!state.sealed) return std::nullopt;
    }
    return std::nullopt;
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_memory_activation.py@7536139:246-267
std::uint64_t NativeCueDirectory::posting_count(
    std::string_view cue, std::int64_t published_row_limit) const {
    const auto head = head_for(cue, published_row_limit);
    return head ? head->count : 0;
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_memory_activation.py@7536139:301-348
std::vector<std::string> NativeCueDirectory::episode_ids_for_cue(
    std::string_view cue, std::int64_t published_row_limit) const {
    const auto head = head_for(cue, published_row_limit);
    if (!head) return {};
    const auto path = posting_path(head->prefix);
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("native_cue_posting_truncated");
    require_data_header(stream, posting_magic, journal_generation_);
    const auto size = std::filesystem::file_size(path);
    std::vector<std::string> identifiers;
    std::uint64_t offset = head->head;
    std::uint64_t remaining = head->count;
    std::optional<std::int64_t> previous_sequence;
    while (remaining) {
        const auto node = read_node_from_stream(stream, offset, size);
        if (node.sequence > published_row_limit ||
            (previous_sequence && node.sequence >= *previous_sequence) ||
            node.cumulative_count != remaining)
            throw std::runtime_error("native_cue_posting_order_changed");
        identifiers.push_back(node.episode_id);
        previous_sequence = node.sequence;
        offset = node.previous;
        --remaining;
        if ((remaining == 0) != (offset == 0))
            throw std::runtime_error("native_cue_posting_count_invalid");
    }
    return identifiers;
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_memory_activation.py@7536139:246-267
std::uint64_t NativeCueDirectory::exact_union_count(
    std::span<const std::string> cues,
    std::int64_t published_row_limit) const {
    struct Cursor {
        std::int64_t sequence;
        std::uint64_t offset;
        std::uint64_t count;
        std::uint8_t prefix;
        bool operator<(const Cursor& other) const {
            return sequence < other.sequence;
        }
    };
    std::priority_queue<Cursor> queue;
    std::array<std::unique_ptr<std::ifstream>, 256> streams;
    std::array<std::uint64_t, 256> sizes{};
    const auto read_cursor_node = [&](std::uint8_t prefix,
                                      std::uint64_t offset) -> PostingNode {
        if (!streams[prefix]) {
            const auto path = posting_path(prefix);
            auto stream = std::make_unique<std::ifstream>(path, std::ios::binary);
            if (!*stream) throw std::runtime_error("native_cue_posting_truncated");
            require_data_header(*stream, posting_magic, journal_generation_);
            sizes[prefix] = std::filesystem::file_size(path);
            streams[prefix] = std::move(stream);
        }
        const auto raw = read_node_from_stream(*streams[prefix], offset,
                                                sizes[prefix]);
        if (raw.previous && raw.previous >= offset)
            throw std::runtime_error("native_cue_posting_corrupt");
        return PostingNode{raw.previous, raw.sequence, raw.cumulative_count,
                           raw.episode_id};
    };
    std::set<std::string> distinct(cues.begin(), cues.end());
    for (const auto& cue : distinct) {
        const auto head = head_for(cue, published_row_limit);
        if (head) queue.push(Cursor{head->sequence, head->head,
                                    head->count, head->prefix});
    }
    std::uint64_t total = 0;
    std::int64_t last_sequence = 0;
    while (!queue.empty()) {
        const auto cursor = queue.top();
        queue.pop();
        const auto node = read_cursor_node(cursor.prefix, cursor.offset);
        if (node.sequence != cursor.sequence ||
            node.cumulative_count != cursor.count)
            throw std::runtime_error("native_cue_posting_order_changed");
        if (node.sequence != last_sequence) {
            ++total;
            last_sequence = node.sequence;
        }
        if (node.previous) {
            const auto prior = read_cursor_node(cursor.prefix, node.previous);
            if (prior.sequence >= node.sequence ||
                prior.cumulative_count + 1 != node.cumulative_count)
                throw std::runtime_error("native_cue_posting_order_changed");
            queue.push(Cursor{prior.sequence, node.previous,
                              prior.cumulative_count, cursor.prefix});
        } else if (cursor.count != 1) {
            throw std::runtime_error("native_cue_posting_count_invalid");
        }
    }
    return total;
}

// SWEGCA: src/swegca_vrs2/cue_shards.py@c06092a:260-360
void NativeCueDirectory::publish(std::int64_t journal_rows,
                                  std::string_view pair_snapshot_id) {
    if (!owner_lock_ || !owner_lock_->locked())
        throw std::runtime_error("native_vrs_owner_lock_required");
    if (journal_rows < 0 || pair_snapshot_id.empty())
        throw std::runtime_error("native_cue_publication_invalid");
    std::vector<std::unique_lock<std::mutex>> prefix_locks;
    prefix_locks.reserve(prefix_mutex_.size());
    for (auto& mutex : prefix_mutex_) prefix_locks.emplace_back(mutex);
    if (failed_.load()) throw std::runtime_error("native_cue_rebuild_required");
    std::lock_guard published(publication_mutex_);
    if (publication_ && journal_rows < publication_->published_rows)
        throw std::runtime_error("native_cue_publication_regressed");
    Json::Object body;
    body.emplace("schema", Json(std::string(publication_schema)));
    body.emplace("journal_generation", Json(journal_generation_));
    body.emplace("published_rows", Json(journal_rows));
    body.emplace("pair_snapshot_id", Json(std::string(pair_snapshot_id)));
    const auto bytes = Json(std::move(body)).canonical();
    try {
        write_atomic_file(directory_ / publication_name,
                          std::as_bytes(std::span(bytes)));
        publication_ = CueDirectoryPublication{
            journal_generation_, journal_rows, std::string(pair_snapshot_id)};
    } catch (...) {
        failed_.store(true);
        throw;
    }
}

// SWEGCA: src/swegca_vrs2/cue_shards.py@c06092a:1-22
std::optional<CueDirectoryPublication> NativeCueDirectory::publication() const {
    std::lock_guard guard(publication_mutex_);
    return publication_;
}

}  // namespace swegca::vrs
