#include "exact_journal_directory.hpp"

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

constexpr std::string_view magic = "VRS2JADDR1";
constexpr std::uint64_t header_bytes = 4096;
constexpr std::uint64_t slot_bytes = 68;
constexpr std::uint64_t state_offset = 80;
constexpr std::array<unsigned, 5> powers{16, 18, 20, 22, 23};
constexpr std::string_view publication_name = "PUBLISHED.json";
constexpr std::string_view publication_schema = "swegca-vrs2-exact-journal-address-v1";
using Bytes = std::array<unsigned char, slot_bytes>;
using Key = std::array<unsigned char, 32>;

struct LevelState {
    std::uint64_t count;
    bool sealed;
};

// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:583-609
std::optional<ExactAddressPublication> read_publication(
    const std::filesystem::path& directory) {
    const auto path = directory / publication_name;
    if (!std::filesystem::exists(path)) return std::nullopt;
    if (std::filesystem::file_size(path) > 4096)
        throw std::runtime_error("exact_journal_publication_invalid");
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("exact_journal_publication_invalid");
    const std::string bytes(std::istreambuf_iterator<char>{stream}, {});
    if (stream.bad()) throw std::runtime_error("exact_journal_publication_invalid");
    try {
        const auto value = Json::parse(bytes);
        if (value.at("schema").string() != publication_schema)
            throw std::runtime_error("exact_journal_publication_invalid");
        auto generation = value.at("journal_generation").string();
        const auto rows = value.at("published_rows").integer();
        auto pair = value.at("pair_snapshot_id").string();
        if (generation.size() != 34 || !generation.starts_with("g-") ||
            rows < 0 || pair.empty())
            throw std::runtime_error("exact_journal_publication_invalid");
        return ExactAddressPublication{std::move(generation), rows, std::move(pair)};
    } catch (const std::exception&) {
        throw std::runtime_error("exact_journal_publication_invalid");
    }
}

// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:592-606
void sync_file(const std::filesystem::path& path) {
#if defined(_WIN32)
    const auto descriptor = _wopen(path.c_str(), _O_BINARY | _O_RDONLY);
    if (descriptor < 0) throw std::runtime_error("exact_journal_address_sync_failed");
    const auto result = _commit(descriptor);
    _close(descriptor);
#else
    const auto descriptor = open(path.c_str(), O_RDONLY);
    if (descriptor < 0) throw std::runtime_error("exact_journal_address_sync_failed");
    const auto result = fsync(descriptor);
    close(descriptor);
#endif
    if (result != 0) throw std::runtime_error("exact_journal_address_sync_failed");
}

// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:362-390
std::uint64_t little_u64(const unsigned char* bytes) {
    std::uint64_t result = 0;
    for (unsigned bit = 0; bit < 8; ++bit)
        result |= static_cast<std::uint64_t>(bytes[bit]) << (bit * 8);
    return result;
}

// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:362-390
void put_u64(unsigned char* bytes, std::uint64_t value) {
    for (unsigned bit = 0; bit < 8; ++bit)
        bytes[bit] = static_cast<unsigned char>((value >> (bit * 8)) & 0xff);
}

// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:311-343
void read_exact(std::istream& stream, char* bytes, std::size_t count) {
    stream.read(bytes, static_cast<std::streamsize>(count));
    if (stream.gcount() != static_cast<std::streamsize>(count))
        throw std::runtime_error("exact_journal_address_segment_invalid");
}

// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:311-343
void write_exact(std::ostream& stream, const char* bytes, std::size_t count) {
    stream.write(bytes, static_cast<std::streamsize>(count));
    if (!stream) throw std::runtime_error("exact_journal_address_write_failed");
}

// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:311-343
std::uint64_t segment_size(unsigned power) {
    return header_bytes + (std::uint64_t{1} << power) * slot_bytes;
}

// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:311-343
void create_segment(const std::filesystem::path& path, unsigned power,
                    unsigned char prefix, std::string_view generation) {
    std::array<unsigned char, header_bytes> header{};
    std::copy(magic.begin(), magic.end(), header.begin());
    header[16] = prefix;
    header[17] = static_cast<unsigned char>(power);
    if (generation.size() != 34)
        throw std::runtime_error("exact_journal_generation_invalid");
    std::copy(generation.begin(), generation.end(), header.begin() + 32);
    write_atomic_file(path, std::as_bytes(std::span(header)));
    std::filesystem::resize_file(path, segment_size(power));
    sync_file(path);
}

// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:311-343
LevelState read_header(std::istream& stream, const std::filesystem::path& path,
                       unsigned power, unsigned char prefix,
                       std::string_view generation) {
    if (std::filesystem::file_size(path) != segment_size(power))
        throw std::runtime_error("exact_journal_address_segment_invalid");
    std::array<unsigned char, 96> header{};
    stream.seekg(0);
    read_exact(stream, reinterpret_cast<char*>(header.data()), header.size());
    if (!std::equal(magic.begin(), magic.end(), header.begin()) ||
        header[16] != prefix || header[17] != power ||
        !std::equal(generation.begin(), generation.end(), header.begin() + 32))
        throw std::runtime_error("exact_journal_address_segment_invalid");
    const auto count = little_u64(header.data() + state_offset);
    const auto sealed = header[state_offset + 8];
    if (count > (std::uint64_t{1} << power) || sealed > 1)
        throw std::runtime_error("exact_journal_address_segment_invalid");
    return LevelState{count, sealed != 0};
}

Bytes read_slot(std::istream& stream, std::uint64_t offset);

// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:362-390
std::pair<std::uint64_t, bool> probe(std::istream& stream, const Key& key,
                                     unsigned power) {
    const auto mask = (std::uint64_t{1} << power) - 1;
    const auto start = little_u64(key.data() + 1) & mask;
    const auto step = (little_u64(key.data() + 9) | 1) & mask;
    for (std::uint64_t count = 0; count <= mask; ++count) {
        const auto position = (start + count * step) & mask;
        const auto offset = header_bytes + position * slot_bytes;
        std::array<unsigned char, 32> found{};
        stream.seekg(static_cast<std::streamoff>(offset));
        read_exact(stream, reinterpret_cast<char*>(found.data()), found.size());
        if (std::all_of(found.begin(), found.end(), [](auto byte) { return byte == 0; })) {
            std::array<unsigned char, slot_bytes - 32> remainder{};
            read_exact(stream, reinterpret_cast<char*>(remainder.data()), remainder.size());
            if (!std::all_of(remainder.begin(), remainder.end(),
                             [](auto byte) { return byte == 0; }))
                throw std::runtime_error("exact_journal_address_slot_corrupt");
            return {offset, false};
        }
        (void)read_slot(stream, offset);
        if (found == key) return {offset, true};
    }
    throw std::runtime_error("exact_journal_address_segment_full");
}

// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:662-684
Bytes read_slot(std::istream& stream, std::uint64_t offset) {
    Bytes bytes{};
    stream.seekg(static_cast<std::streamoff>(offset));
    read_exact(stream, reinterpret_cast<char*>(bytes.data()), bytes.size());
    std::uint32_t expected = 0;
    for (unsigned bit = 0; bit < 4; ++bit)
        expected |= static_cast<std::uint32_t>(bytes[64 + bit]) << (bit * 8);
    const auto actual = crc32(0, reinterpret_cast<const Bytef*>(bytes.data()), 64);
    if (expected != actual)
        throw std::runtime_error("exact_journal_address_slot_corrupt");
    return bytes;
}

// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:496-609
Bytes encode_slot(const Key& key, const OriginalJournalAddress& address) {
    Bytes bytes{};
    std::copy(key.begin(), key.end(), bytes.begin());
    put_u64(bytes.data() + 32, static_cast<std::uint64_t>(address.sequence));
    put_u64(bytes.data() + 40, address.frame.byte_offset);
    put_u64(bytes.data() + 48, static_cast<std::uint64_t>(address.frame.first_sequence));
    put_u64(bytes.data() + 56, static_cast<std::uint64_t>(address.frame.last_sequence));
    const auto checksum = crc32(0, reinterpret_cast<const Bytef*>(bytes.data()), 64);
    for (unsigned bit = 0; bit < 4; ++bit)
        bytes[64 + bit] = static_cast<unsigned char>((checksum >> (bit * 8)) & 0xff);
    return bytes;
}

// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:691-713
OriginalJournalAddress decode_slot(const Bytes& bytes, std::string_view generation) {
    const auto sequence = little_u64(bytes.data() + 32);
    const auto offset = little_u64(bytes.data() + 40);
    const auto first = little_u64(bytes.data() + 48);
    const auto last = little_u64(bytes.data() + 56);
    if (sequence > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
        first > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
        last > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
        first < 1 || first > sequence || sequence > last || offset < 8)
        throw std::runtime_error("exact_journal_address_slot_corrupt");
    return OriginalJournalAddress{JournalFrameAddress{
        std::string(generation), "head.vrsj", offset,
        static_cast<std::int64_t>(first), static_cast<std::int64_t>(last)},
        static_cast<std::int64_t>(sequence)};
}

}  // namespace

// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:72-83
ExactJournalDirectory::Key ExactJournalDirectory::key_of(
    std::string_view episode_id) {
    if (!episode_id.starts_with("memory:") || episode_id.size() != 71)
        throw std::runtime_error("invalid_exact_experience_address");
    Key key{};
    for (std::size_t at = 0; at < key.size(); ++at) {
        const auto digit = [](char value) -> int {
            if (value >= '0' && value <= '9') return value - '0';
            if (value >= 'a' && value <= 'f') return value - 'a' + 10;
            if (value >= 'A' && value <= 'F') return value - 'A' + 10;
            return -1;
        };
        const auto high = digit(episode_id[7 + at * 2]);
        const auto low = digit(episode_id[8 + at * 2]);
        if (high < 0 || low < 0)
            throw std::runtime_error("invalid_exact_experience_address");
        key[at] = static_cast<unsigned char>((high << 4) | low);
    }
    if (std::all_of(key.begin(), key.end(), [](auto byte) { return byte == 0; }))
        throw std::runtime_error("reserved_exact_experience_address");
    return key;
}

// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:311-343
std::filesystem::path ExactJournalDirectory::segment_path(const Key& key,
                                                            unsigned power) const {
    std::ostringstream name;
    name << "address-p" << power << '-' << std::hex << std::setw(2)
         << std::setfill('0') << static_cast<unsigned>(key[0]) << ".vrs";
    return directory_ / name.str();
}

// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:190-223
ExactJournalDirectory::ExactJournalDirectory(std::filesystem::path directory,
                                             std::string journal_generation,
                                             OwnerLock* owner_lock)
    : directory_(std::move(directory)),
      journal_generation_(std::move(journal_generation)), owner_lock_(owner_lock) {
    if (journal_generation_.size() != 34 ||
        !journal_generation_.starts_with("g-"))
        throw std::runtime_error("exact_journal_generation_invalid");
    if (owner_lock_) {
        if (!owner_lock_->locked())
            throw std::runtime_error("native_vrs_owner_lock_required");
        if (std::filesystem::create_directories(directory_))
            std::filesystem::permissions(directory_,
                std::filesystem::perms::owner_all,
                std::filesystem::perm_options::replace);
    } else if (!std::filesystem::is_directory(directory_)) {
        throw std::runtime_error("exact_journal_address_directory_missing");
    }
    publication_ = read_publication(directory_);
    if (publication_) {
        if (publication_->journal_generation != journal_generation_)
            throw std::runtime_error("exact_journal_generation_changed");
    } else if (!owner_lock_ || !std::filesystem::is_empty(directory_)) {
        throw std::runtime_error("exact_journal_address_unpublished_directory");
    }
}

// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:496-609
void ExactJournalDirectory::put(std::string_view episode_id,
                                const OriginalJournalAddress& address) {
    const auto key = key_of(episode_id);
    std::lock_guard guard(prefix_mutex_[key[0]]);
    if (failed_.load())
        throw std::runtime_error("exact_journal_address_directory_failed");
    if (!owner_lock_ || !owner_lock_->locked())
        throw std::runtime_error("native_vrs_owner_lock_required");
    if (address.frame.generation != journal_generation_ || address.sequence < 1 ||
        address.frame.first_sequence < 1 ||
        address.frame.first_sequence > address.sequence ||
        address.sequence > address.frame.last_sequence ||
        address.frame.byte_offset < 8)
        throw std::runtime_error("exact_journal_address_invalid");
    try {
        for (const auto power : powers) {
            const auto path = segment_path(key, power);
            if (!std::filesystem::exists(path))
                create_segment(path, power, key[0], journal_generation_);
            std::fstream stream(path, std::ios::in | std::ios::out | std::ios::binary);
            if (!stream) throw std::runtime_error("exact_journal_address_segment_invalid");
            auto state = read_header(stream, path, power, key[0], journal_generation_);
            const auto [offset, exists] = probe(stream, key, power);
            if (exists) {
                const auto stored = decode_slot(read_slot(stream, offset), journal_generation_);
                if (stored.sequence != address.sequence ||
                    stored.frame.byte_offset != address.frame.byte_offset ||
                    stored.frame.first_sequence != address.frame.first_sequence ||
                    stored.frame.last_sequence != address.frame.last_sequence)
                    throw std::runtime_error("exact_replay_address_reassigned");
                return;
            }
            if (state.sealed) continue;
            const auto bytes = encode_slot(key, address);
            stream.seekp(static_cast<std::streamoff>(offset + key.size()));
            write_exact(stream, reinterpret_cast<const char*>(bytes.data() + key.size()),
                        bytes.size() - key.size());
            stream.seekp(static_cast<std::streamoff>(offset));
            write_exact(stream, reinterpret_cast<const char*>(bytes.data()), key.size());
            ++state.count;
            const auto threshold = ((std::uint64_t{1} << power) * 7) / 10;
            state.sealed = state.count >= threshold;
            std::array<unsigned char, 9> state_bytes{};
            put_u64(state_bytes.data(), state.count);
            state_bytes[8] = static_cast<unsigned char>(state.sealed);
            stream.seekp(static_cast<std::streamoff>(state_offset));
            write_exact(stream, reinterpret_cast<const char*>(state_bytes.data()),
                        state_bytes.size());
            stream.flush();
            if (!stream) throw std::runtime_error("exact_journal_address_write_failed");
            stream.close();
            sync_file(path);
            return;
        }
        throw std::runtime_error("exact_replay_directory_capacity_exceeded");
    } catch (...) {
        failed_.store(true);
        throw;
    }
}

// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:691-713
std::optional<OriginalJournalAddress> ExactJournalDirectory::find(
    std::string_view episode_id, std::int64_t published_row_limit) const {
    const auto key = key_of(episode_id);
    std::lock_guard guard(prefix_mutex_[key[0]]);
    if (failed_.load()) throw std::runtime_error("exact_journal_address_directory_failed");
    if (published_row_limit < 0)
        throw std::runtime_error("exact_journal_published_limit_invalid");
    if (!owner_lock_) {
        std::lock_guard published(publication_mutex_);
        if (!publication_ || published_row_limit > publication_->published_rows)
            throw std::runtime_error("exact_journal_unpublished_read");
    }
    for (const auto power : powers) {
        const auto path = segment_path(key, power);
        if (!std::filesystem::exists(path)) return std::nullopt;
        std::ifstream stream(path, std::ios::binary);
        if (!stream) throw std::runtime_error("exact_journal_address_segment_invalid");
        const auto state = read_header(stream, path, power, key[0], journal_generation_);
        const auto [offset, exists] = probe(stream, key, power);
        if (exists) {
            auto address = decode_slot(read_slot(stream, offset), journal_generation_);
            if (address.sequence > published_row_limit) return std::nullopt;
            return address;
        }
        if (!state.sealed) return std::nullopt;
    }
    return std::nullopt;
}

// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:190-223
bool ExactJournalDirectory::fresh() const {
    if (failed_.load()) throw std::runtime_error("exact_journal_address_directory_failed");
    return std::filesystem::is_empty(directory_);
}

// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:583-609
void ExactJournalDirectory::publish(std::int64_t journal_rows,
                                    std::string_view pair_snapshot_id) {
    if (!owner_lock_ || !owner_lock_->locked())
        throw std::runtime_error("native_vrs_owner_lock_required");
    if (journal_rows < 0 || pair_snapshot_id.empty())
        throw std::runtime_error("exact_journal_publication_invalid");
    std::vector<std::unique_lock<std::mutex>> prefix_locks;
    prefix_locks.reserve(prefix_mutex_.size());
    for (auto& mutex : prefix_mutex_) prefix_locks.emplace_back(mutex);
    if (failed_.load()) throw std::runtime_error("exact_journal_address_directory_failed");
    std::lock_guard published(publication_mutex_);
    if (publication_ && journal_rows < publication_->published_rows)
        throw std::runtime_error("exact_journal_publication_regressed");
    Json::Object body;
    body.emplace("schema", Json(std::string(publication_schema)));
    body.emplace("journal_generation", Json(journal_generation_));
    body.emplace("published_rows", Json(journal_rows));
    body.emplace("pair_snapshot_id", Json(std::string(pair_snapshot_id)));
    const auto bytes = Json(std::move(body)).canonical();
    try {
        write_atomic_file(directory_ / publication_name,
                          std::as_bytes(std::span(bytes)));
        publication_ = ExactAddressPublication{
            journal_generation_, journal_rows, std::string(pair_snapshot_id)};
    } catch (...) {
        failed_.store(true);
        throw;
    }
}

// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:583-609
std::optional<ExactAddressPublication> ExactJournalDirectory::publication() const {
    std::lock_guard guard(publication_mutex_);
    return publication_;
}

}  // namespace swegca::vrs
