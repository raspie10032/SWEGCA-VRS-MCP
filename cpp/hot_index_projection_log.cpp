#include "hot_index_projection_log.hpp"

#include "journal_files.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace swegca::vrs {
namespace {

constexpr std::string_view magic = "VRSHLOG1";
constexpr std::size_t header_bytes = 64;
constexpr std::uint64_t maximum_frame_bytes = 16 * 1024 * 1024 + 80;

// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:72-83
std::uint8_t prefix_of(std::string_view episode_id) {
    if (!episode_id.starts_with("memory:") || episode_id.size() != 71)
        throw std::runtime_error("hot_index_projection_address_invalid");
    const auto nibble = [](char value) -> int {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        if (value >= 'A' && value <= 'F') return value - 'A' + 10;
        return -1;
    };
    const auto high = nibble(episode_id[7]);
    const auto low = nibble(episode_id[8]);
    if (high < 0 || low < 0 ||
        !std::all_of(episode_id.begin() + 9, episode_id.end(),
                     [&](char digit) { return nibble(digit) >= 0; }))
        throw std::runtime_error("hot_index_projection_address_invalid");
    return static_cast<std::uint8_t>((high << 4) | low);
}

// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:311-343
std::array<std::byte, header_bytes> file_header(std::string_view generation) {
    if (generation.size() != 34 || !generation.starts_with("g-"))
        throw std::runtime_error("hot_index_projection_generation_invalid");
    std::array<std::byte, header_bytes> header{};
    for (std::size_t at = 0; at < magic.size(); ++at)
        header[at] = std::byte(magic[at]);
    for (std::size_t at = 0; at < generation.size(); ++at)
        header[magic.size() + at] = std::byte(generation[at]);
    return header;
}

// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:311-343
void require_header(std::ifstream& stream, std::string_view generation) {
    std::array<std::byte, header_bytes> actual{};
    stream.seekg(0);
    stream.read(reinterpret_cast<char*>(actual.data()), actual.size());
    if (!stream || actual != file_header(generation))
        throw std::runtime_error("hot_index_projection_file_invalid");
}

// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:592-606
void sync_file(const std::filesystem::path& path) {
#if defined(_WIN32)
    const auto descriptor = _wopen(path.c_str(), _O_BINARY | _O_RDONLY);
    if (descriptor < 0) throw std::runtime_error("hot_index_projection_sync_failed");
    const auto result = _commit(descriptor);
    _close(descriptor);
#else
    const auto descriptor = open(path.c_str(), O_RDONLY);
    if (descriptor < 0) throw std::runtime_error("hot_index_projection_sync_failed");
    const auto result = fsync(descriptor);
    close(descriptor);
#endif
    if (result != 0) throw std::runtime_error("hot_index_projection_sync_failed");
}

}  // namespace

// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:190-223
HotIndexProjectionLog::HotIndexProjectionLog(
    std::filesystem::path directory, std::string journal_generation,
    OwnerLock* owner_lock)
    : directory_(std::move(directory)),
      journal_generation_(std::move(journal_generation)),
      owner_lock_(owner_lock) {
    (void)file_header(journal_generation_);
    if (owner_lock_) {
        if (!owner_lock_->locked())
            throw std::runtime_error("native_vrs_owner_lock_required");
        if (std::filesystem::create_directories(directory_))
            std::filesystem::permissions(directory_,
                std::filesystem::perms::owner_all,
                std::filesystem::perm_options::replace);
        if (!std::filesystem::is_empty(directory_))
            throw std::runtime_error("hot_index_projection_rebuild_required");
    } else if (!std::filesystem::is_directory(directory_)) {
        throw std::runtime_error("hot_index_projection_directory_missing");
    }
}

// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:311-343
std::filesystem::path HotIndexProjectionLog::file_for(std::uint8_t prefix) const {
    std::ostringstream name;
    name << "headers-" << std::hex << std::setw(2) << std::setfill('0')
         << static_cast<unsigned>(prefix) << ".vrs";
    return directory_ / name.str();
}

// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:496-609
HotProjectionAddress HotIndexProjectionLog::append(
    const HotIndexProjectionRow& row) {
    const auto prefix = prefix_of(row.header.episode_id);
    std::lock_guard guard(prefix_mutex_[prefix]);
    if (!owner_lock_ || !owner_lock_->locked())
        throw std::runtime_error("native_vrs_owner_lock_required");
    if (failed_.load())
        throw std::runtime_error("hot_index_projection_rebuild_required");
    try {
        const auto frame = encode_hot_index_projection(row);
        const auto path = file_for(prefix);
        if (!std::filesystem::exists(path)) {
            const auto header = file_header(journal_generation_);
            write_atomic_file(path, std::span<const std::byte>(header));
        }
        const auto offset = std::filesystem::file_size(path);
        if (offset < header_bytes ||
            offset > std::numeric_limits<std::uint64_t>::max() - frame.size())
            throw std::runtime_error("hot_index_projection_file_invalid");
        std::ifstream check(path, std::ios::binary);
        if (!check) throw std::runtime_error("hot_index_projection_file_invalid");
        require_header(check, journal_generation_);
        check.close();
        std::ofstream stream(path, std::ios::binary | std::ios::app);
        if (!stream) throw std::runtime_error("hot_index_projection_write_failed");
        stream.write(reinterpret_cast<const char*>(frame.data()), frame.size());
        stream.flush();
        if (!stream) throw std::runtime_error("hot_index_projection_write_failed");
        stream.close();
        sync_file(path);
        return HotProjectionAddress{journal_generation_, prefix, offset,
            static_cast<std::uint32_t>(frame.size()), row.journal_sequence};
    } catch (...) {
        failed_.store(true);
        throw;
    }
}

// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:691-713
HotIndexProjectionRow HotIndexProjectionLog::read_at(
    const HotProjectionAddress& address, std::string_view expected_episode_id,
    std::int64_t published_row_limit) const {
    const auto prefix = prefix_of(expected_episode_id);
    std::lock_guard guard(prefix_mutex_[prefix]);
    if (failed_.load() || address.journal_generation != journal_generation_ ||
        address.prefix != prefix || address.journal_sequence < 1 ||
        address.journal_sequence > published_row_limit ||
        address.byte_offset < header_bytes || address.byte_length < 80 ||
        address.byte_length > maximum_frame_bytes)
        throw std::runtime_error("hot_index_projection_address_invalid");
    const auto path = file_for(prefix);
    if (!std::filesystem::is_regular_file(path) ||
        address.byte_offset > std::filesystem::file_size(path) ||
        address.byte_length > std::filesystem::file_size(path) - address.byte_offset)
        throw std::runtime_error("hot_index_projection_address_invalid");
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("hot_index_projection_file_invalid");
    require_header(stream, journal_generation_);
    std::vector<std::byte> frame(address.byte_length);
    stream.seekg(static_cast<std::streamoff>(address.byte_offset));
    stream.read(reinterpret_cast<char*>(frame.data()), frame.size());
    if (!stream) throw std::runtime_error("hot_index_projection_file_invalid");
    auto row = decode_hot_index_projection(frame);
    if (row.journal_sequence != address.journal_sequence ||
        row.header.episode_id != expected_episode_id)
        throw std::runtime_error("hot_index_projection_address_invalid");
    return row;
}

}  // namespace swegca::vrs
