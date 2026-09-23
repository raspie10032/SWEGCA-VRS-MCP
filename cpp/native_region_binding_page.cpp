#include "native_region_binding_page.hpp"

#include "journal_files.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <span>
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

constexpr std::string_view file_magic = "VRS2RBF1";
constexpr std::string_view page_magic = "VRS2RBP1";
constexpr std::uint64_t file_header_bytes = 64;
constexpr std::uint64_t page_header_bytes = 64;
constexpr std::uint64_t payload_bytes =
    NativeGraphPageMap::records_per_page * region_binding_record_bytes;
constexpr std::uint64_t physical_page_bytes =
    page_header_bytes + payload_bytes;

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
        throw std::runtime_error("region_binding_checksum_too_large");
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

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:49-62
void sync_path(const std::filesystem::path& path) {
#if defined(_WIN32)
    const auto descriptor = _wopen(path.c_str(), _O_BINARY | _O_RDONLY);
    if (descriptor < 0) throw std::runtime_error("region_binding_sync_failed");
    const auto result = _commit(descriptor);
    _close(descriptor);
#else
    const auto descriptor = open(path.c_str(), O_RDONLY);
    if (descriptor < 0) throw std::runtime_error("region_binding_sync_failed");
    const auto result = fsync(descriptor);
    close(descriptor);
#endif
    if (result != 0) throw std::runtime_error("region_binding_sync_failed");
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:49-62
void read_exact(std::istream& input, std::byte* out, std::size_t count) {
    input.read(reinterpret_cast<char*>(out),
               static_cast<std::streamsize>(count));
    if (input.gcount() != static_cast<std::streamsize>(count))
        throw std::runtime_error("region_binding_file_truncated");
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:49-62
void write_exact(std::ostream& output, const std::byte* source,
                 std::size_t count) {
    output.write(reinterpret_cast<const char*>(source),
                 static_cast<std::streamsize>(count));
    if (!output) throw std::runtime_error("region_binding_write_failed");
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
        throw std::runtime_error("region_binding_file_generation_changed");
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:91-127
std::array<std::byte, page_header_bytes> page_header(
    std::string_view generation, std::uint32_t page_id,
    std::uint32_t count, const std::byte* payload) {
    std::array<std::byte, page_header_bytes> header{};
    for (std::size_t at = 0; at < page_magic.size(); ++at)
        header[at] = static_cast<std::byte>(page_magic[at]);
    for (std::size_t at = 0; at < generation.size(); ++at)
        header[8 + at] = static_cast<std::byte>(generation[at]);
    put_u32(header.data() + 42, page_id);
    put_u32(header.data() + 46, count);
    put_u32(header.data() + 50, checksum(payload, payload_bytes));
    put_u32(header.data() + 54, checksum(header.data(), 54));
    return header;
}

}  // namespace

// SWEGCA: src/swegca_vrs2/store.py@c06092a:542-575
std::array<std::byte, region_binding_record_bytes>
encode_region_binding_record(RegionBindingRecord value) {
    if (!value.present && (value.component != 0 || value.local != 0))
        throw std::runtime_error("region_binding_absent_record_invalid");
    std::array<std::byte, region_binding_record_bytes> bytes{};
    put_u32(bytes.data(), value.component);
    put_u32(bytes.data() + 4, value.local);
    bytes[8] = static_cast<std::byte>(value.present ? 1 : 0);
    put_u32(bytes.data() + 12, checksum(bytes.data(), 12));
    return bytes;
}

// SWEGCA: src/swegca_vrs2/store.py@c06092a:542-575
RegionBindingRecord decode_region_binding_record(
    const std::array<std::byte, region_binding_record_bytes>& bytes) {
    if ((bytes[8] != std::byte{} && bytes[8] != std::byte{1}) ||
        bytes[9] != std::byte{} || bytes[10] != std::byte{} ||
        bytes[11] != std::byte{} ||
        get_u32(bytes.data() + 12) != checksum(bytes.data(), 12))
        throw std::runtime_error("region_binding_record_corrupt");
    RegionBindingRecord value{get_u32(bytes.data()),
                              get_u32(bytes.data() + 4),
                              bytes[8] == std::byte{1}};
    if (!value.present && (value.component != 0 || value.local != 0))
        throw std::runtime_error("region_binding_absent_record_invalid");
    return value;
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:49-62
NativeRegionBindingPageFile::NativeRegionBindingPageFile(
    std::filesystem::path path, std::string journal_generation,
    OwnerLock* writer_lock)
    : path_(std::move(path)),
      journal_generation_(std::move(journal_generation)),
      writer_lock_(writer_lock) {
    if (path_.parent_path().empty() || !generation_name(journal_generation_))
        throw std::runtime_error("region_binding_file_invalid");
    if (writer_lock_) {
        require_writer();
        if (std::filesystem::create_directories(path_.parent_path()))
            std::filesystem::permissions(
                path_.parent_path(), std::filesystem::perms::owner_all,
                std::filesystem::perm_options::replace);
        if (!std::filesystem::exists(path_)) {
            const auto header = file_header(journal_generation_);
            write_atomic_file(path_, std::span<const std::byte>(header));
        }
    }
    if (!std::filesystem::is_regular_file(path_) ||
        std::filesystem::is_symlink(path_))
        throw std::runtime_error("region_binding_file_invalid");
    std::ifstream input(path_, std::ios::binary);
    if (!input) throw std::runtime_error("region_binding_file_invalid");
    require_file_header(input, journal_generation_);
    const auto size = std::filesystem::file_size(path_);
    if (size < file_header_bytes)
        throw std::runtime_error("region_binding_file_truncated");
    const auto tail = (size - file_header_bytes) % physical_page_bytes;
    if (tail != 0) {
        if (!writer_lock_)
            throw std::runtime_error("region_binding_file_truncated");
        std::filesystem::resize_file(path_, size - tail);
        sync_path(path_);
    }
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:109-127
void NativeRegionBindingPageFile::require_writer() const {
    if (!writer_lock_ || !writer_lock_->locked())
        throw std::runtime_error("native_vrs_owner_lock_required");
}

// SWEGCA: src/swegca_vrs2/store.py@c06092a:542-575
std::uint64_t NativeRegionBindingPageFile::append(
    const NativeRegionBindingPage& page) {
    require_writer();
    if (page.page_id > NativeGraphPageMap::maximum_page_id ||
        page.valid_records == 0 ||
        page.valid_records > NativeGraphPageMap::records_per_page)
        throw std::runtime_error("region_binding_page_shape_invalid");
    std::array<std::byte, payload_bytes> payload{};
    for (std::uint32_t at = 0; at < page.valid_records; ++at) {
        const auto encoded = encode_region_binding_record(page.records[at]);
        std::copy(encoded.begin(), encoded.end(),
                  payload.begin() + at * encoded.size());
    }
    const auto header = page_header(journal_generation_, page.page_id,
                                    page.valid_records, payload.data());
    std::lock_guard guard(mutex_);
    const auto position = std::filesystem::file_size(path_);
    if (position < file_header_bytes ||
        (position - file_header_bytes) % physical_page_bytes != 0 ||
        position > static_cast<std::uint64_t>(
                       std::numeric_limits<std::streamoff>::max()) -
                       physical_page_bytes)
        throw std::runtime_error("region_binding_file_tail_invalid");
    std::ofstream output(path_, std::ios::binary | std::ios::app);
    if (!output) throw std::runtime_error("region_binding_write_failed");
    write_exact(output, header.data(), header.size());
    write_exact(output, payload.data(), payload.size());
    output.flush();
    if (!output) throw std::runtime_error("region_binding_write_failed");
    return position;
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:49-62
void NativeRegionBindingPageFile::sync() const {
    require_writer();
    std::lock_guard guard(mutex_);
    sync_path(path_);
}

// SWEGCA: src/swegca_vrs2/store.py@c06092a:297-310
NativeRegionBindingPage NativeRegionBindingPageFile::read(
    std::uint64_t offset, std::uint32_t expected_page_id) const {
    if (expected_page_id > NativeGraphPageMap::maximum_page_id ||
        offset < file_header_bytes ||
        (offset - file_header_bytes) % physical_page_bytes != 0)
        throw std::runtime_error("region_binding_page_address_invalid");
    const auto size = std::filesystem::file_size(path_);
    if (offset > size || physical_page_bytes > size - offset ||
        offset > static_cast<std::uint64_t>(
                     std::numeric_limits<std::streamoff>::max()) ||
        physical_page_bytes >
            static_cast<std::uint64_t>(
                std::numeric_limits<std::streamoff>::max()) - offset)
        throw std::runtime_error("region_binding_file_truncated");
    std::ifstream input(path_, std::ios::binary);
    if (!input) throw std::runtime_error("region_binding_file_invalid");
    input.seekg(static_cast<std::streamoff>(offset));
    std::array<std::byte, page_header_bytes> header{};
    std::array<std::byte, payload_bytes> payload{};
    read_exact(input, header.data(), header.size());
    read_exact(input, payload.data(), payload.size());
    const auto count = get_u32(header.data() + 46);
    const auto expected = page_header(journal_generation_, expected_page_id,
                                      count, payload.data());
    if (header != expected || count == 0 ||
        count > NativeGraphPageMap::records_per_page)
        throw std::runtime_error("region_binding_page_invalid");
    NativeRegionBindingPage result;
    result.page_id = expected_page_id;
    result.valid_records = count;
    for (std::uint32_t at = 0; at < count; ++at) {
        std::array<std::byte, region_binding_record_bytes> encoded{};
        std::copy_n(payload.begin() + at * encoded.size(), encoded.size(),
                    encoded.begin());
        result.records[at] = decode_region_binding_record(encoded);
    }
    for (std::size_t at = count * region_binding_record_bytes;
         at < payload.size(); ++at)
        if (payload[at] != std::byte{})
            throw std::runtime_error("region_binding_page_padding_invalid");
    return result;
}

}  // namespace swegca::vrs
