#include "native_region_topology_catalog.hpp"

#include "journal_files.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
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

constexpr std::string_view file_magic = "VRS2RCF1";
constexpr std::string_view record_magic = "VRS2RCR1";
constexpr std::uint64_t file_header_bytes = 64;
constexpr std::uint64_t record_bytes = 384;
constexpr std::size_t maximum_name_bytes = 128;

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:26-29
void put_u32(std::byte* out, std::uint32_t value) {
    for (unsigned at = 0; at < 4; ++at)
        out[at] = static_cast<std::byte>((value >> (8 * at)) & 0xff);
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:26-29
std::uint32_t get_u32(const std::byte* in) {
    std::uint32_t value = 0;
    for (unsigned at = 0; at < 4; ++at)
        value |= std::uint32_t(std::to_integer<unsigned char>(in[at])) <<
                 (8 * at);
    return value;
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:1-10
std::uint32_t checksum(const std::byte* bytes, std::size_t count) {
    if (count > std::numeric_limits<uInt>::max())
        throw std::runtime_error("region_catalog_checksum_too_large");
    return crc32(0, reinterpret_cast<const Bytef*>(bytes),
                 static_cast<uInt>(count));
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:91-127
bool generation_name(std::string_view value) {
    return value.size() == 34 && value.starts_with("g-") &&
        std::all_of(value.begin() + 2, value.end(), [](char digit) {
            return (digit >= '0' && digit <= '9') ||
                   (digit >= 'a' && digit <= 'f');
        });
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:44-45
bool digest_id(std::string_view value) {
    return value.size() == 64 &&
        std::all_of(value.begin(), value.end(), [](char digit) {
            return (digit >= '0' && digit <= '9') ||
                   (digit >= 'a' && digit <= 'f');
        });
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:49-62
void sync_path(const std::filesystem::path& path) {
#if defined(_WIN32)
    const auto descriptor = _wopen(path.c_str(), _O_BINARY | _O_RDONLY);
    if (descriptor < 0) throw std::runtime_error("region_catalog_sync_failed");
    const auto result = _commit(descriptor);
    _close(descriptor);
#else
    const auto descriptor = open(path.c_str(), O_RDONLY);
    if (descriptor < 0) throw std::runtime_error("region_catalog_sync_failed");
    const auto result = fsync(descriptor);
    close(descriptor);
#endif
    if (result != 0) throw std::runtime_error("region_catalog_sync_failed");
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:49-62
void read_exact(std::istream& input, std::byte* out, std::size_t count) {
    input.read(reinterpret_cast<char*>(out),
               static_cast<std::streamsize>(count));
    if (input.gcount() != static_cast<std::streamsize>(count))
        throw std::runtime_error("region_catalog_file_truncated");
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:49-62
void write_exact(std::ostream& output, const std::byte* source,
                 std::size_t count) {
    output.write(reinterpret_cast<const char*>(source),
                 static_cast<std::streamsize>(count));
    if (!output) throw std::runtime_error("region_catalog_write_failed");
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:49-62
std::array<std::byte, file_header_bytes> file_header(
    std::string_view generation) {
    std::array<std::byte, file_header_bytes> header{};
    for (std::size_t at = 0; at < file_magic.size(); ++at)
        header[at] = static_cast<std::byte>(file_magic[at]);
    for (std::size_t at = 0; at < generation.size(); ++at)
        header[8 + at] = static_cast<std::byte>(generation[at]);
    put_u32(header.data() + 44, checksum(header.data(), 44));
    return header;
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:49-62
void require_file_header(std::istream& input, std::string_view generation) {
    std::array<std::byte, file_header_bytes> actual{};
    input.seekg(0);
    read_exact(input, actual.data(), actual.size());
    if (actual != file_header(generation))
        throw std::runtime_error("region_catalog_generation_changed");
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:91-127
bool safe_topology_name(std::string_view name, std::uint32_t component,
                        std::string_view topology_id) {
    if (name.empty() || name.size() > maximum_name_bytes ||
        name.find('/') != std::string_view::npos ||
        name.find('\\') != std::string_view::npos ||
        !digest_id(topology_id))
        return false;
    const auto prefix = "rgt-" + std::to_string(component) + "-" +
        std::string(topology_id.substr(0, 16)) + "-";
    if (!name.starts_with(prefix) || !name.ends_with(".vrs") ||
        name.size() != prefix.size() + 16 + 4)
        return false;
    const auto random_hex = name.substr(prefix.size(), 16);
    if (!std::all_of(random_hex.begin(), random_hex.end(), [](char digit) {
            return (digit >= '0' && digit <= '9') ||
                   (digit >= 'a' && digit <= 'f');
        }))
        return false;
    const std::filesystem::path path(name);
    return !path.has_parent_path() && path.filename() == path;
}

// SWEGCA: src/swegca_vrs2/store.py@c06092a:542-577
std::array<std::byte, record_bytes> encode_record(
    const NativeRegionTopologyCatalogRecord& value,
    std::string_view generation) {
    if (!generation_name(generation) || !digest_id(value.vrs_snapshot_id) ||
        !digest_id(value.topology_id) ||
        !safe_topology_name(value.file_name, value.component,
                            value.topology_id))
        throw std::runtime_error("region_catalog_record_invalid");
    std::array<std::byte, record_bytes> bytes{};
    for (std::size_t at = 0; at < record_magic.size(); ++at)
        bytes[at] = static_cast<std::byte>(record_magic[at]);
    for (std::size_t at = 0; at < generation.size(); ++at)
        bytes[8 + at] = static_cast<std::byte>(generation[at]);
    put_u32(bytes.data() + 42, value.component);
    for (std::size_t at = 0; at < value.vrs_snapshot_id.size(); ++at)
        bytes[46 + at] = static_cast<std::byte>(value.vrs_snapshot_id[at]);
    for (std::size_t at = 0; at < value.topology_id.size(); ++at)
        bytes[110 + at] = static_cast<std::byte>(value.topology_id[at]);
    put_u32(bytes.data() + 174,
            static_cast<std::uint32_t>(value.file_name.size()));
    for (std::size_t at = 0; at < value.file_name.size(); ++at)
        bytes[178 + at] = static_cast<std::byte>(value.file_name[at]);
    put_u32(bytes.data() + 380, checksum(bytes.data(), 380));
    return bytes;
}

// SWEGCA: src/swegca_vrs2/store.py@c06092a:542-577
NativeRegionTopologyCatalogRecord decode_record(
    const std::array<std::byte, record_bytes>& bytes,
    std::string_view generation) {
    const auto name_size = get_u32(bytes.data() + 174);
    if (name_size == 0 || name_size > maximum_name_bytes ||
        get_u32(bytes.data() + 380) != checksum(bytes.data(), 380))
        throw std::runtime_error("region_catalog_record_invalid");
    for (std::size_t at = 0; at < record_magic.size(); ++at)
        if (bytes[at] != static_cast<std::byte>(record_magic[at]))
            throw std::runtime_error("region_catalog_record_invalid");
    for (std::size_t at = 0; at < generation.size(); ++at)
        if (bytes[8 + at] != static_cast<std::byte>(generation[at]))
            throw std::runtime_error("region_catalog_generation_changed");
    for (std::size_t at = 178 + name_size; at < 380; ++at)
        if (bytes[at] != std::byte{})
            throw std::runtime_error("region_catalog_record_invalid");
    std::string vrs, topology, name;
    vrs.reserve(64);
    topology.reserve(64);
    name.reserve(name_size);
    for (std::size_t at = 0; at < 64; ++at) {
        vrs.push_back(static_cast<char>(
            std::to_integer<unsigned char>(bytes[46 + at])));
        topology.push_back(static_cast<char>(
            std::to_integer<unsigned char>(bytes[110 + at])));
    }
    for (std::size_t at = 0; at < name_size; ++at)
        name.push_back(static_cast<char>(
            std::to_integer<unsigned char>(bytes[178 + at])));
    NativeRegionTopologyCatalogRecord result{
        get_u32(bytes.data() + 42), std::move(vrs),
        std::move(topology), std::move(name)};
    if (!digest_id(result.vrs_snapshot_id) || !digest_id(result.topology_id) ||
        !safe_topology_name(result.file_name, result.component,
                            result.topology_id))
        throw std::runtime_error("region_catalog_record_invalid");
    return result;
}

}  // namespace

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:49-62
NativeRegionTopologyCatalog::NativeRegionTopologyCatalog(
    std::filesystem::path path, std::filesystem::path topology_directory,
    std::string journal_generation, OwnerLock* writer_lock)
    : path_(std::move(path)),
      topology_directory_(std::move(topology_directory)),
      journal_generation_(std::move(journal_generation)),
      writer_lock_(writer_lock) {
    if (path_.parent_path().empty() || topology_directory_.empty() ||
        !generation_name(journal_generation_))
        throw std::runtime_error("region_catalog_file_invalid");
    if (writer_lock_) {
        require_writer();
        if (std::filesystem::create_directories(path_.parent_path()))
            std::filesystem::permissions(
                path_.parent_path(), std::filesystem::perms::owner_all,
                std::filesystem::perm_options::replace);
        if (std::filesystem::create_directories(topology_directory_))
            std::filesystem::permissions(
                topology_directory_, std::filesystem::perms::owner_all,
                std::filesystem::perm_options::replace);
        if (!std::filesystem::exists(path_)) {
            const auto header = file_header(journal_generation_);
            write_atomic_file(path_, std::span<const std::byte>(header));
        }
    }
    if (!std::filesystem::is_regular_file(path_) ||
        std::filesystem::is_symlink(path_) ||
        !std::filesystem::is_directory(topology_directory_) ||
        std::filesystem::is_symlink(topology_directory_))
        throw std::runtime_error("region_catalog_file_invalid");
    std::ifstream input(path_, std::ios::binary);
    if (!input) throw std::runtime_error("region_catalog_file_invalid");
    require_file_header(input, journal_generation_);
    const auto size = std::filesystem::file_size(path_);
    if (size < file_header_bytes)
        throw std::runtime_error("region_catalog_file_truncated");
    const auto tail = (size - file_header_bytes) % record_bytes;
    if (tail != 0) {
        if (!writer_lock_)
            throw std::runtime_error("region_catalog_file_truncated");
        std::filesystem::resize_file(path_, size - tail);
        sync_path(path_);
    }
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:109-127
void NativeRegionTopologyCatalog::require_writer() const {
    if (!writer_lock_ || !writer_lock_->locked())
        throw std::runtime_error("native_vrs_owner_lock_required");
}

// SWEGCA: src/swegca_vrs2/store.py@c06092a:542-577
std::uint64_t NativeRegionTopologyCatalog::append(
    const NativeRegionTopologyFile& topology) {
    require_writer();
    if (!topology.converged() ||
        topology.journal_generation() != journal_generation_ ||
        !std::filesystem::equivalent(topology.path().parent_path(),
                                     topology_directory_) ||
        std::filesystem::equivalent(topology.path(), path_))
        throw std::runtime_error("region_catalog_topology_source_changed");
    NativeRegionTopologyCatalogRecord value{
        topology.component(), topology.vrs_snapshot_id(),
        topology.topology_id(), topology.path().filename().string()};
    const auto encoded = encode_record(value, journal_generation_);
    std::lock_guard guard(mutex_);
    const auto offset = std::filesystem::file_size(path_);
    const auto stream_limit = static_cast<std::uint64_t>(
        std::numeric_limits<std::streamoff>::max());
    if (offset < file_header_bytes ||
        (offset - file_header_bytes) % record_bytes != 0 ||
        offset > stream_limit || record_bytes > stream_limit - offset)
        throw std::runtime_error("region_catalog_file_tail_invalid");
    std::ofstream output(path_, std::ios::binary | std::ios::app);
    if (!output) throw std::runtime_error("region_catalog_write_failed");
    write_exact(output, encoded.data(), encoded.size());
    output.flush();
    if (!output) throw std::runtime_error("region_catalog_write_failed");
    return offset;
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:49-62
void NativeRegionTopologyCatalog::sync() const {
    require_writer();
    std::lock_guard guard(mutex_);
    sync_path(path_);
}

// SWEGCA: src/swegca_vrs2/store.py@c06092a:542-577
NativeRegionTopologyCatalogRecord NativeRegionTopologyCatalog::read_record(
    std::uint64_t offset) const {
    if (offset < file_header_bytes ||
        (offset - file_header_bytes) % record_bytes != 0)
        throw std::runtime_error("region_catalog_address_invalid");
    const auto size = std::filesystem::file_size(path_);
    const auto stream_limit = static_cast<std::uint64_t>(
        std::numeric_limits<std::streamoff>::max());
    if (offset > size || record_bytes > size - offset ||
        offset > stream_limit || record_bytes > stream_limit - offset)
        throw std::runtime_error("region_catalog_file_truncated");
    std::ifstream input(path_, std::ios::binary);
    if (!input) throw std::runtime_error("region_catalog_file_invalid");
    input.seekg(static_cast<std::streamoff>(offset));
    std::array<std::byte, record_bytes> encoded{};
    read_exact(input, encoded.data(), encoded.size());
    return decode_record(encoded, journal_generation_);
}

// SWEGCA: src/swegca_vrs2/store.py@c06092a:542-577
std::shared_ptr<const NativeRegionTopologyFile>
NativeRegionTopologyCatalog::open_topology(
    std::uint64_t offset, std::uint32_t expected_component) const {
    const auto record = read_record(offset);
    if (record.component != expected_component)
        throw std::runtime_error("region_catalog_component_changed");
    return NativeRegionTopologyFile::open(
        topology_directory_ / record.file_name, journal_generation_,
        record.component, record.vrs_snapshot_id, record.topology_id);
}

}  // namespace swegca::vrs
