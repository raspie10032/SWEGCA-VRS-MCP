#include "native_graph_page_file.hpp"

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

constexpr std::uint64_t file_header_bytes = 64;
constexpr std::uint64_t page_header_bytes = 64;
constexpr std::string_view node_file_magic = "VRS2GNF1";
constexpr std::string_view edge_file_magic = "VRS2GEF1";
constexpr std::string_view node_page_magic = "VRS2GNP1";
constexpr std::string_view edge_page_magic = "VRS2GEP1";

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:26-29
void put_u32(std::byte* target, std::uint32_t value) {
    for (unsigned at = 0; at < 4; ++at)
        target[at] = static_cast<std::byte>((value >> (at * 8)) & 0xff);
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:26-29
std::uint32_t get_u32(const std::byte* source) {
    std::uint32_t value = 0;
    for (unsigned at = 0; at < 4; ++at)
        value |= std::uint32_t(std::to_integer<unsigned char>(source[at])) << (at * 8);
    return value;
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:1-10
std::uint32_t checksum(const std::byte* source, std::size_t count) {
    if (count > std::numeric_limits<uInt>::max())
        throw std::runtime_error("graph_numeric_page_too_large");
    return crc32(0, reinterpret_cast<const Bytef*>(source),
                 static_cast<uInt>(count));
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:49-62
void sync_path(const std::filesystem::path& path) {
#if defined(_WIN32)
    const auto descriptor = _wopen(path.c_str(), _O_BINARY | _O_RDONLY);
    if (descriptor < 0) throw std::runtime_error("graph_numeric_sync_failed");
    const auto result = _commit(descriptor);
    _close(descriptor);
#else
    const auto descriptor = open(path.c_str(), O_RDONLY);
    if (descriptor < 0) throw std::runtime_error("graph_numeric_sync_failed");
    const auto result = fsync(descriptor);
    close(descriptor);
#endif
    if (result != 0) throw std::runtime_error("graph_numeric_sync_failed");
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:49-62
void read_exact(std::istream& input, std::byte* target, std::size_t count) {
    input.read(reinterpret_cast<char*>(target),
               static_cast<std::streamsize>(count));
    if (input.gcount() != static_cast<std::streamsize>(count))
        throw std::runtime_error("graph_numeric_page_truncated");
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:49-62
void write_exact(std::ostream& output, const std::byte* source,
                 std::size_t count) {
    output.write(reinterpret_cast<const char*>(source),
                 static_cast<std::streamsize>(count));
    if (!output) throw std::runtime_error("graph_numeric_write_failed");
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:49-62
void require_file_header(std::istream& input, GraphPageKind kind,
                         std::string_view generation) {
    std::array<std::byte, file_header_bytes> found{};
    input.seekg(0);
    read_exact(input, found.data(), found.size());
    std::array<std::byte, file_header_bytes> expected{};
    const auto magic = kind == GraphPageKind::node ?
        node_file_magic : edge_file_magic;
    for (std::size_t at = 0; at < magic.size(); ++at)
        expected[at] = static_cast<std::byte>(magic[at]);
    for (std::size_t at = 0; at < generation.size(); ++at)
        expected[16 + at] = static_cast<std::byte>(generation[at]);
    if (found != expected)
        throw std::runtime_error("graph_numeric_file_generation_changed");
}

}  // namespace

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:91-127
NativeGraphPageFile::NativeGraphPageFile(
    std::filesystem::path path, std::string journal_generation,
    GraphPageKind kind, OwnerLock* writer_lock)
    : path_(std::move(path)), journal_generation_(std::move(journal_generation)),
      kind_(kind), writer_lock_(writer_lock) {
    if (journal_generation_.size() != 34 ||
        !journal_generation_.starts_with("g-") || path_.parent_path().empty())
        throw std::runtime_error("graph_numeric_generation_invalid");
    if (writer_lock_) {
        require_writer();
        if (std::filesystem::create_directories(path_.parent_path()))
            std::filesystem::permissions(
                path_.parent_path(), std::filesystem::perms::owner_all,
                std::filesystem::perm_options::replace);
        if (!std::filesystem::exists(path_)) {
            std::array<std::byte, file_header_bytes> header{};
            const auto magic = kind_ == GraphPageKind::node ?
                node_file_magic : edge_file_magic;
            for (std::size_t at = 0; at < magic.size(); ++at)
                header[at] = static_cast<std::byte>(magic[at]);
            for (std::size_t at = 0; at < journal_generation_.size(); ++at)
                header[16 + at] = static_cast<std::byte>(journal_generation_[at]);
            write_atomic_file(path_, std::span<const std::byte>(header));
        }
    }
    std::ifstream input(path_, std::ios::binary);
    if (!input) throw std::runtime_error("graph_numeric_file_missing");
    require_file_header(input, kind_, journal_generation_);
    const auto size = std::filesystem::file_size(path_);
    if (size < file_header_bytes)
        throw std::runtime_error("graph_numeric_file_truncated");
    const auto tail = (size - file_header_bytes) % page_bytes();
    if (tail != 0 && writer_lock_) {
        // The page map cannot point at an incomplete page. A committed map
        // update is written only after complete page sync.
        std::filesystem::resize_file(path_, size - tail);
        sync_path(path_);
    }
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:109-127
void NativeGraphPageFile::require_writer() const {
    if (!writer_lock_ || !writer_lock_->locked())
        throw std::runtime_error("native_vrs_owner_lock_required");
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_delta.py@7536139:141-163
std::uint64_t NativeGraphPageFile::page_bytes() const {
    const auto record = kind_ == GraphPageKind::node ?
        graph_numeric_node_record_bytes : graph_numeric_edge_record_bytes;
    return page_header_bytes + NativeGraphPageMap::records_per_page * record;
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_delta.py@7536139:138-169
std::uint64_t NativeGraphPageFile::append_encoded(
    std::uint32_t page_id, std::uint32_t count,
    const std::byte* payload, std::size_t payload_bytes) {
    require_writer();
    if (page_id > NativeGraphPageMap::maximum_page_id ||
        count == 0 || count > NativeGraphPageMap::records_per_page ||
        payload_bytes != page_bytes() - page_header_bytes)
        throw std::runtime_error("graph_numeric_page_shape_invalid");
    std::lock_guard guard(writer_mutex_);
    const auto position = std::filesystem::file_size(path_);
    if (position < file_header_bytes ||
        (position - file_header_bytes) % page_bytes() != 0 ||
        position > static_cast<std::uint64_t>(
            std::numeric_limits<std::streamoff>::max()) - page_bytes())
        throw std::runtime_error("graph_numeric_file_tail_invalid");
    std::array<std::byte, page_header_bytes> header{};
    const auto magic = kind_ == GraphPageKind::node ?
        node_page_magic : edge_page_magic;
    for (std::size_t at = 0; at < magic.size(); ++at)
        header[at] = static_cast<std::byte>(magic[at]);
    for (std::size_t at = 0; at < journal_generation_.size(); ++at)
        header[8 + at] = static_cast<std::byte>(journal_generation_[at]);
    put_u32(header.data() + 42, page_id);
    put_u32(header.data() + 46, count);
    put_u32(header.data() + 50, checksum(payload, payload_bytes));
    put_u32(header.data() + 54, checksum(header.data(), 54));
    std::ofstream output(path_, std::ios::binary | std::ios::app);
    if (!output) throw std::runtime_error("graph_numeric_write_failed");
    write_exact(output, header.data(), header.size());
    write_exact(output, payload, payload_bytes);
    output.flush();
    if (!output) throw std::runtime_error("graph_numeric_write_failed");
    return position;
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_delta.py@7536139:138-169
std::uint64_t NativeGraphPageFile::append(const NativeGraphNodePage& page) {
    if (kind_ != GraphPageKind::node ||
        page.valid_records > NativeGraphPageMap::records_per_page)
        throw std::runtime_error("graph_numeric_page_kind_invalid");
    std::array<std::byte, NativeGraphPageMap::records_per_page *
                          graph_numeric_node_record_bytes> payload{};
    for (std::uint32_t at = 0; at < page.valid_records; ++at) {
        const auto encoded = encode_graph_numeric_node(page.records[at]);
        std::copy(encoded.begin(), encoded.end(),
                  payload.begin() + at * encoded.size());
    }
    return append_encoded(page.page_id, page.valid_records,
                          payload.data(), payload.size());
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_delta.py@7536139:138-169
std::uint64_t NativeGraphPageFile::append(const NativeGraphEdgePage& page) {
    if (kind_ != GraphPageKind::edge ||
        page.valid_records > NativeGraphPageMap::records_per_page)
        throw std::runtime_error("graph_numeric_page_kind_invalid");
    std::array<std::byte, NativeGraphPageMap::records_per_page *
                          graph_numeric_edge_record_bytes> payload{};
    for (std::uint32_t at = 0; at < page.valid_records; ++at) {
        const auto encoded = encode_graph_numeric_edge(page.records[at]);
        std::copy(encoded.begin(), encoded.end(),
                  payload.begin() + at * encoded.size());
    }
    return append_encoded(page.page_id, page.valid_records,
                          payload.data(), payload.size());
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:49-62
void NativeGraphPageFile::sync() const {
    require_writer();
    std::lock_guard guard(writer_mutex_);
    sync_path(path_);
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:34-76
void NativeGraphPageFile::read_encoded(
    std::uint64_t offset, std::uint32_t expected_page_id,
    std::byte* payload, std::size_t payload_bytes,
    std::uint32_t& count) const {
    if (expected_page_id > NativeGraphPageMap::maximum_page_id ||
        payload_bytes != page_bytes() - page_header_bytes ||
        offset < file_header_bytes ||
        (offset - file_header_bytes) % page_bytes() != 0)
        throw std::runtime_error("graph_numeric_page_address_invalid");
    const auto size = std::filesystem::file_size(path_);
    if (offset > size || page_bytes() > size - offset)
        throw std::runtime_error("graph_numeric_page_truncated");
    std::ifstream input(path_, std::ios::binary);
    if (!input) throw std::runtime_error("graph_numeric_file_missing");
    input.seekg(static_cast<std::streamoff>(offset));
    std::array<std::byte, page_header_bytes> header{};
    read_exact(input, header.data(), header.size());
    const auto magic = kind_ == GraphPageKind::node ?
        node_page_magic : edge_page_magic;
    for (std::size_t at = 0; at < magic.size(); ++at)
        if (header[at] != static_cast<std::byte>(magic[at]))
            throw std::runtime_error("graph_numeric_page_header_invalid");
    for (std::size_t at = 0; at < journal_generation_.size(); ++at)
        if (header[8 + at] != static_cast<std::byte>(journal_generation_[at]))
            throw std::runtime_error("graph_numeric_page_generation_changed");
    count = get_u32(header.data() + 46);
    if (get_u32(header.data() + 42) != expected_page_id ||
        count == 0 || count > NativeGraphPageMap::records_per_page ||
        get_u32(header.data() + 54) != checksum(header.data(), 54))
        throw std::runtime_error("graph_numeric_page_header_invalid");
    for (std::size_t at = 58; at < header.size(); ++at)
        if (header[at] != std::byte{})
            throw std::runtime_error("graph_numeric_page_header_invalid");
    read_exact(input, payload, payload_bytes);
    if (get_u32(header.data() + 50) != checksum(payload, payload_bytes))
        throw std::runtime_error("graph_numeric_page_corrupt");
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:34-76
NativeGraphNodePage NativeGraphPageFile::read_node(
    std::uint64_t offset, std::uint32_t expected_page_id) const {
    if (kind_ != GraphPageKind::node)
        throw std::runtime_error("graph_numeric_page_kind_invalid");
    std::array<std::byte, NativeGraphPageMap::records_per_page *
                          graph_numeric_node_record_bytes> payload{};
    NativeGraphNodePage page;
    page.page_id = expected_page_id;
    read_encoded(offset, expected_page_id, payload.data(), payload.size(),
                 page.valid_records);
    for (std::uint32_t at = 0; at < page.valid_records; ++at) {
        const auto start = payload.data() + at * graph_numeric_node_record_bytes;
        page.records[at] = decode_graph_numeric_node(
            std::span<const std::byte, graph_numeric_node_record_bytes>(
                start, graph_numeric_node_record_bytes));
    }
    return page;
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:34-76
NativeGraphEdgePage NativeGraphPageFile::read_edge(
    std::uint64_t offset, std::uint32_t expected_page_id) const {
    if (kind_ != GraphPageKind::edge)
        throw std::runtime_error("graph_numeric_page_kind_invalid");
    std::array<std::byte, NativeGraphPageMap::records_per_page *
                          graph_numeric_edge_record_bytes> payload{};
    NativeGraphEdgePage page;
    page.page_id = expected_page_id;
    read_encoded(offset, expected_page_id, payload.data(), payload.size(),
                 page.valid_records);
    for (std::uint32_t at = 0; at < page.valid_records; ++at) {
        const auto start = payload.data() + at * graph_numeric_edge_record_bytes;
        page.records[at] = decode_graph_numeric_edge(
            std::span<const std::byte, graph_numeric_edge_record_bytes>(
                start, graph_numeric_edge_record_bytes));
    }
    return page;
}

}  // namespace swegca::vrs
