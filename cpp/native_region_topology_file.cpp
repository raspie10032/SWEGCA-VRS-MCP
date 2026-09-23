#include "native_region_topology_file.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
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

constexpr std::string_view file_magic = "VRS2RGT1";
constexpr std::string_view block_magic = "VRS2RGP1";
constexpr std::uint64_t file_header_bytes = 512;
constexpr std::uint64_t block_header_bytes = 64;
constexpr std::uint64_t block_payload_bytes = 4096;
constexpr std::uint64_t block_bytes =
    block_header_bytes + block_payload_bytes;
constexpr std::size_t section_count = 7;
constexpr std::array<std::uint32_t, section_count> expected_widths{
    4, 4, 8, 4, 8, 8, 4};

struct Descriptor {
    std::uint64_t count = 0;
    std::uint32_t width = 0;
    std::uint32_t blocks = 0;
    std::uint64_t start = 0;
};

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

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:26-29
void put_u64(std::byte* out, std::uint64_t value) {
    for (unsigned at = 0; at < 8; ++at)
        out[at] = static_cast<std::byte>((value >> (8 * at)) & 0xff);
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:26-29
std::uint64_t get_u64(const std::byte* in) {
    std::uint64_t value = 0;
    for (unsigned at = 0; at < 8; ++at)
        value |= std::uint64_t(std::to_integer<unsigned char>(in[at])) <<
                 (8 * at);
    return value;
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:1-10
std::uint32_t checksum(const std::byte* bytes, std::size_t count) {
    if (count > std::numeric_limits<uInt>::max())
        throw std::runtime_error("region_topology_checksum_too_large");
    return crc32(0, reinterpret_cast<const Bytef*>(bytes),
                 static_cast<uInt>(count));
}

// The block header CRC excludes its own four-byte field and includes the
// topology prefix that prevents a valid block from another file being spliced.
// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:1-10
std::uint32_t block_header_checksum(
    const std::array<std::byte, block_header_bytes>& header) {
    auto value = crc32(0, reinterpret_cast<const Bytef*>(header.data()), 24);
    return crc32(value, reinterpret_cast<const Bytef*>(header.data() + 28),
                 static_cast<uInt>(header.size() - 28));
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
void sync_path(const std::filesystem::path& path, bool directory) {
#if defined(_WIN32)
    if (directory) return;
    const auto descriptor = _wopen(path.c_str(), _O_BINARY | _O_RDONLY);
    if (descriptor < 0) throw std::runtime_error("region_topology_sync_failed");
    const auto result = _commit(descriptor);
    _close(descriptor);
#else
    const auto descriptor = open(path.c_str(), O_RDONLY);
    if (descriptor < 0) throw std::runtime_error("region_topology_sync_failed");
    const auto result = fsync(descriptor);
    close(descriptor);
#endif
    if (result != 0) throw std::runtime_error("region_topology_sync_failed");
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:49-62
void read_exact(std::istream& input, std::byte* out, std::size_t count) {
    input.read(reinterpret_cast<char*>(out),
               static_cast<std::streamsize>(count));
    if (input.gcount() != static_cast<std::streamsize>(count))
        throw std::runtime_error("region_topology_file_truncated");
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:49-62
void write_exact(std::ostream& output, const std::byte* source,
                 std::size_t count) {
    output.write(reinterpret_cast<const char*>(source),
                 static_cast<std::streamsize>(count));
    if (!output) throw std::runtime_error("region_topology_write_failed");
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:91-127
std::filesystem::path fresh_path(const std::filesystem::path& directory,
                                 std::uint32_t component,
                                 std::string_view topology) {
    std::random_device random;
    constexpr char digits[] = "0123456789abcdef";
    for (unsigned attempt = 0; attempt < 16; ++attempt) {
        std::string suffix;
        suffix.reserve(16);
        for (unsigned byte = 0; byte < 8; ++byte) {
            const auto value = static_cast<unsigned char>(random());
            suffix.push_back(digits[value >> 4]);
            suffix.push_back(digits[value & 15]);
        }
        const auto path = directory /
            ("rgt-" + std::to_string(component) + "-" +
             std::string(topology.substr(0, 16)) + "-" + suffix + ".vrs");
        if (!std::filesystem::exists(path)) return path;
    }
    throw std::runtime_error("region_topology_name_exhausted");
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_connectivity_regions.py@7536139:138-157
std::array<Descriptor, section_count> descriptors(
    const ConnectivityRegions& topology) {
    const auto terms = topology.terms().size();
    const auto memberships = topology.member_regions().size();
    const auto regions = topology.region_offsets().empty() ? 0 :
        topology.region_offsets().size() - 1;
    const std::array<std::uint64_t, section_count> counts{
        terms, terms, topology.member_offsets().size(), memberships,
        topology.member_weights().size(), topology.region_offsets().size(),
        topology.region_nodes().size()};
    std::array<Descriptor, section_count> result{};
    std::uint64_t start = 0;
    for (std::size_t at = 0; at < result.size(); ++at) {
        if (counts[at] > std::numeric_limits<std::uint64_t>::max() /
                             expected_widths[at])
            throw std::runtime_error("region_topology_array_too_large");
        const auto bytes = counts[at] * expected_widths[at];
        const auto blocks = bytes == 0 ? 0 :
            (bytes - 1) / block_payload_bytes + 1;
        if (blocks > std::numeric_limits<std::uint32_t>::max())
            throw std::runtime_error("region_topology_array_too_large");
        result[at] = Descriptor{counts[at], expected_widths[at],
                                static_cast<std::uint32_t>(blocks), start};
        start += blocks;
    }
    if (topology.core_labels().size() != terms ||
        topology.member_offsets().size() != terms + 1 ||
        topology.member_regions().size() != topology.member_weights().size() ||
        topology.region_offsets().size() != regions + 1 ||
        topology.region_nodes().size() != memberships ||
        topology.member_offsets().back() != memberships ||
        topology.region_offsets().back() != topology.region_nodes().size())
        throw std::runtime_error("region_topology_array_shape_invalid");
    return result;
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:91-127
std::array<std::byte, file_header_bytes> make_header(
    std::string_view generation, std::uint32_t component,
    const ConnectivityRegions& topology,
    const std::array<Descriptor, section_count>& sections) {
    std::array<std::byte, file_header_bytes> header{};
    for (std::size_t at = 0; at < file_magic.size(); ++at)
        header[at] = static_cast<std::byte>(file_magic[at]);
    for (std::size_t at = 0; at < generation.size(); ++at)
        header[8 + at] = static_cast<std::byte>(generation[at]);
    for (std::size_t at = 0; at < topology.vrs_snapshot_id().size(); ++at)
        header[48 + at] = static_cast<std::byte>(topology.vrs_snapshot_id()[at]);
    for (std::size_t at = 0; at < topology.topology_id().size(); ++at)
        header[112 + at] = static_cast<std::byte>(topology.topology_id()[at]);
    put_u32(header.data() + 176, component);
    put_u32(header.data() + 180, topology.converged() ? 1 : 0);
    for (std::size_t at = 0; at < sections.size(); ++at) {
        const auto offset = 184 + at * 24;
        put_u64(header.data() + offset, sections[at].count);
        put_u32(header.data() + offset + 8, sections[at].width);
        put_u32(header.data() + offset + 12, sections[at].blocks);
        put_u64(header.data() + offset + 16, sections[at].start);
    }
    const auto total = sections.empty() ? 0 :
        sections.back().start + sections.back().blocks;
    put_u64(header.data() + 352, total);
    put_u32(header.data() + 360, checksum(header.data(), 360));
    return header;
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:91-127
std::array<std::byte, block_header_bytes> make_block_header(
    std::uint32_t section, std::uint32_t block, std::uint32_t valid,
    const std::array<std::byte, block_payload_bytes>& payload,
    std::string_view topology_id) {
    std::array<std::byte, block_header_bytes> header{};
    for (std::size_t at = 0; at < block_magic.size(); ++at)
        header[at] = static_cast<std::byte>(block_magic[at]);
    put_u32(header.data() + 8, section);
    put_u32(header.data() + 12, block);
    put_u32(header.data() + 16, valid);
    put_u32(header.data() + 20, checksum(payload.data(), valid));
    for (std::size_t at = 0; at < 32; ++at)
        header[32 + at] = static_cast<std::byte>(topology_id[at]);
    put_u32(header.data() + 24, block_header_checksum(header));
    return header;
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_connectivity_regions.py@7536139:138-157
template <typename Value>
void write_section(std::ostream& output, std::uint32_t section,
                   std::span<const Value> values,
                   std::string_view topology_id) {
    static_assert(sizeof(Value) == 4 || sizeof(Value) == 8);
    std::array<std::byte, block_payload_bytes> payload{};
    std::uint32_t used = 0, block = 0;
    auto flush = [&]() {
        const auto header = make_block_header(section, block++, used,
                                              payload, topology_id);
        write_exact(output, header.data(), header.size());
        write_exact(output, payload.data(), payload.size());
        payload.fill(std::byte{});
        used = 0;
    };
    for (const auto value : values) {
        if constexpr (sizeof(Value) == 4) {
            std::uint32_t encoded;
            if constexpr (std::is_same_v<Value, float>)
                encoded = std::bit_cast<std::uint32_t>(value);
            else encoded = static_cast<std::uint32_t>(value);
            put_u32(payload.data() + used, encoded);
        } else {
            std::uint64_t encoded;
            if constexpr (std::is_same_v<Value, double>)
                encoded = std::bit_cast<std::uint64_t>(value);
            else encoded = static_cast<std::uint64_t>(value);
            put_u64(payload.data() + used, encoded);
        }
        used += sizeof(Value);
        if (used == block_payload_bytes) flush();
    }
    if (used != 0) flush();
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:91-127
std::array<Descriptor, section_count> parse_header(
    const std::array<std::byte, file_header_bytes>& header,
    std::string_view generation, std::uint32_t component,
    std::string_view vrs_snapshot_id, std::string_view topology_id,
    bool& converged) {
    for (std::size_t at = 0; at < file_magic.size(); ++at)
        if (header[at] != static_cast<std::byte>(file_magic[at]))
            throw std::runtime_error("region_topology_header_invalid");
    for (std::size_t at = 0; at < generation.size(); ++at)
        if (header[8 + at] != static_cast<std::byte>(generation[at]))
            throw std::runtime_error("region_topology_generation_changed");
    for (std::size_t at = 0; at < vrs_snapshot_id.size(); ++at)
        if (header[48 + at] != static_cast<std::byte>(vrs_snapshot_id[at]))
            throw std::runtime_error("region_topology_vrs_changed");
    for (std::size_t at = 0; at < topology_id.size(); ++at)
        if (header[112 + at] != static_cast<std::byte>(topology_id[at]))
            throw std::runtime_error("region_topology_id_changed");
    for (std::size_t at = 42; at < 48; ++at)
        if (header[at] != std::byte{})
            throw std::runtime_error("region_topology_header_invalid");
    if (get_u32(header.data() + 176) != component ||
        get_u32(header.data() + 180) > 1 ||
        get_u32(header.data() + 360) != checksum(header.data(), 360))
        throw std::runtime_error("region_topology_header_invalid");
    for (std::size_t at = 364; at < header.size(); ++at)
        if (header[at] != std::byte{})
            throw std::runtime_error("region_topology_header_invalid");
    converged = get_u32(header.data() + 180) != 0;
    std::array<Descriptor, section_count> result{};
    std::uint64_t expected_start = 0;
    for (std::size_t at = 0; at < result.size(); ++at) {
        const auto offset = 184 + at * 24;
        result[at] = Descriptor{
            get_u64(header.data() + offset),
            get_u32(header.data() + offset + 8),
            get_u32(header.data() + offset + 12),
            get_u64(header.data() + offset + 16)};
        if (result[at].width != expected_widths[at] ||
            result[at].start != expected_start ||
            result[at].count > std::numeric_limits<std::uint64_t>::max() /
                                   result[at].width)
            throw std::runtime_error("region_topology_header_invalid");
        const auto bytes = result[at].count * result[at].width;
        const auto blocks = bytes == 0 ? 0 :
            (bytes - 1) / block_payload_bytes + 1;
        if (blocks != result[at].blocks)
            throw std::runtime_error("region_topology_header_invalid");
        expected_start += result[at].blocks;
    }
    if (get_u64(header.data() + 352) != expected_start)
        throw std::runtime_error("region_topology_header_invalid");
    return result;
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:91-127
std::array<std::byte, block_payload_bytes> read_block(
    std::ifstream& input, const Descriptor& descriptor,
    std::uint32_t section, std::uint32_t local,
    std::string_view topology_id) {
    if (local >= descriptor.blocks)
        throw std::runtime_error("region_topology_block_address_invalid");
    const auto global = descriptor.start + local;
    if (global >
            (std::numeric_limits<std::uint64_t>::max() - file_header_bytes) /
                block_bytes)
        throw std::runtime_error("region_topology_block_address_invalid");
    const auto offset = file_header_bytes + global * block_bytes;
    const auto stream_limit = static_cast<std::uint64_t>(
        std::numeric_limits<std::streamoff>::max());
    if (offset > stream_limit || block_bytes > stream_limit - offset)
        throw std::runtime_error("region_topology_block_address_invalid");
    input.clear();
    input.seekg(static_cast<std::streamoff>(offset));
    std::array<std::byte, block_header_bytes> header{};
    std::array<std::byte, block_payload_bytes> payload{};
    read_exact(input, header.data(), header.size());
    read_exact(input, payload.data(), payload.size());
    for (std::size_t at = 0; at < block_magic.size(); ++at)
        if (header[at] != static_cast<std::byte>(block_magic[at]))
            throw std::runtime_error("region_topology_block_invalid");
    const auto expected_valid = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(block_payload_bytes,
            descriptor.count * descriptor.width -
                std::uint64_t(local) * block_payload_bytes));
    if (get_u32(header.data() + 8) != section ||
        get_u32(header.data() + 12) != local ||
        get_u32(header.data() + 16) != expected_valid ||
        get_u32(header.data() + 20) != checksum(payload.data(), expected_valid) ||
        get_u32(header.data() + 24) != block_header_checksum(header))
        throw std::runtime_error("region_topology_block_invalid");
    for (std::size_t at = 0; at < 4; ++at)
        if (header[28 + at] != std::byte{})
            throw std::runtime_error("region_topology_block_invalid");
    for (std::size_t at = 0; at < 32; ++at)
        if (header[32 + at] != static_cast<std::byte>(topology_id[at]))
            throw std::runtime_error("region_topology_block_changed");
    for (std::size_t at = expected_valid; at < payload.size(); ++at)
        if (payload[at] != std::byte{})
            throw std::runtime_error("region_topology_block_invalid");
    return payload;
}

// Cold validation streams each physical section once. It verifies every
// block without reopening the file for each 4 KiB range or retaining a
// complete array in memory.
// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_connectivity_regions.py@7536139:138-157
template <typename Visitor>
void visit_values(std::ifstream& input, const Descriptor& descriptor,
                  std::uint32_t section, std::string_view topology_id,
                  Visitor&& visitor) {
    std::uint64_t visited = 0;
    for (std::uint32_t block = 0; block < descriptor.blocks; ++block) {
        const auto payload = read_block(input, descriptor, section, block,
                                        topology_id);
        const auto remaining = descriptor.count - visited;
        const auto in_block = std::min<std::uint64_t>(
            remaining, block_payload_bytes / descriptor.width);
        for (std::uint64_t at = 0; at < in_block; ++at) {
            const auto byte = at * descriptor.width;
            visitor(descriptor.width == 4 ?
                        std::uint64_t{get_u32(payload.data() + byte)} :
                        get_u64(payload.data() + byte),
                    visited + at);
        }
        visited += in_block;
    }
    if (visited != descriptor.count)
        throw std::runtime_error("region_topology_array_changed");
}

}  // namespace

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_connectivity_regions.py@7536139:159-199
std::shared_ptr<const NativeRegionTopologyFile>
NativeRegionTopologyFile::create(
    const std::filesystem::path& directory,
    std::string journal_generation,
    std::uint32_t component,
    const ConnectivityRegions& topology,
    OwnerLock& owner) {
    if (!owner.locked())
        throw std::runtime_error("native_vrs_owner_lock_required");
    if (directory.empty() || !generation_name(journal_generation) ||
        !digest_id(topology.vrs_snapshot_id()) ||
        !digest_id(topology.topology_id()) || !topology.converged())
        throw std::runtime_error("region_topology_source_invalid");
    if (std::filesystem::create_directories(directory))
        std::filesystem::permissions(
            directory, std::filesystem::perms::owner_all,
            std::filesystem::perm_options::replace);
    const auto sections = descriptors(topology);
    const auto header = make_header(journal_generation, component,
                                    topology, sections);
    const auto final_path = fresh_path(directory, component,
                                       topology.topology_id());
    const auto temporary = final_path.string() + ".tmp";
    try {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("region_topology_write_failed");
        write_exact(output, header.data(), header.size());
        write_section(output, 0,
                      std::span<const std::uint32_t>(topology.terms()),
                      topology.topology_id());
        write_section(output, 1,
                      std::span<const std::uint32_t>(topology.core_labels()),
                      topology.topology_id());
        write_section(output, 2,
                      std::span<const std::uint64_t>(topology.member_offsets()),
                      topology.topology_id());
        write_section(output, 3,
                      std::span<const std::uint32_t>(topology.member_regions()),
                      topology.topology_id());
        write_section(output, 4,
                      std::span<const double>(topology.member_weights()),
                      topology.topology_id());
        write_section(output, 5,
                      std::span<const std::uint64_t>(topology.region_offsets()),
                      topology.topology_id());
        write_section(output, 6,
                      std::span<const std::uint32_t>(topology.region_nodes()),
                      topology.topology_id());
        output.flush();
        if (!output) throw std::runtime_error("region_topology_write_failed");
        output.close();
        if (!output) throw std::runtime_error("region_topology_write_failed");
        sync_path(temporary, false);
        std::filesystem::rename(temporary, final_path);
        sync_path(directory, true);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
    return open(final_path, std::move(journal_generation), component,
                topology.vrs_snapshot_id(), topology.topology_id());
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_connectivity_regions.py@7536139:138-203
std::shared_ptr<const NativeRegionTopologyFile>
NativeRegionTopologyFile::open(
    std::filesystem::path path,
    std::string journal_generation,
    std::uint32_t component,
    std::string vrs_snapshot_id,
    std::string topology_id) {
    if (path.parent_path().empty() ||
        path.filename().string().rfind("rgt-", 0) != 0 ||
        path.extension() != ".vrs" || !generation_name(journal_generation) ||
        !digest_id(vrs_snapshot_id) || !digest_id(topology_id) ||
        !std::filesystem::is_regular_file(path) ||
        std::filesystem::is_symlink(path))
        throw std::runtime_error("region_topology_file_invalid");
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("region_topology_file_invalid");
    std::array<std::byte, file_header_bytes> header{};
    read_exact(input, header.data(), header.size());
    bool converged = false;
    const auto parsed = parse_header(header, journal_generation, component,
                                     vrs_snapshot_id, topology_id, converged);
    const auto total_blocks = parsed.back().start + parsed.back().blocks;
    if (total_blocks >
            (std::numeric_limits<std::uint64_t>::max() - file_header_bytes) /
                block_bytes ||
        std::filesystem::file_size(path) !=
            file_header_bytes + total_blocks * block_bytes)
        throw std::runtime_error("region_topology_file_invalid");
    auto result = std::shared_ptr<NativeRegionTopologyFile>(
        new NativeRegionTopologyFile());
    result->path_ = std::move(path);
    result->journal_generation_ = std::move(journal_generation);
    result->component_ = component;
    result->vrs_snapshot_id_ = std::move(vrs_snapshot_id);
    result->topology_id_ = std::move(topology_id);
    result->converged_ = converged;
    for (std::size_t at = 0; at < parsed.size(); ++at) {
        result->counts_[at] = parsed[at].count;
        result->widths_[at] = parsed[at].width;
        result->blocks_[at] = parsed[at].blocks;
        result->starts_[at] = parsed[at].start;
    }
    result->validate_file();
    return result;
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_connectivity_regions.py@7536139:138-157
std::vector<std::uint64_t> NativeRegionTopologyFile::read_values(
    Section section, std::uint64_t first, std::uint64_t count) const {
    const auto index = static_cast<std::size_t>(section);
    if (index >= section_count || first > counts_[index] ||
        count > counts_[index] - first ||
        count > std::numeric_limits<std::size_t>::max())
        throw std::out_of_range("region_topology_array_address_invalid");
    std::vector<std::uint64_t> result;
    result.reserve(static_cast<std::size_t>(count));
    if (count == 0) return result;
    std::ifstream input(path_, std::ios::binary);
    if (!input) throw std::runtime_error("region_topology_file_invalid");
    const Descriptor descriptor{counts_[index], widths_[index],
                                blocks_[index], starts_[index]};
    const auto first_byte = first * descriptor.width;
    const auto last_byte = (first + count) * descriptor.width;
    const auto first_block = first_byte / block_payload_bytes;
    const auto last_block = (last_byte - 1) / block_payload_bytes;
    for (auto block = first_block; block <= last_block; ++block) {
        const auto payload = read_block(
            input, descriptor, static_cast<std::uint32_t>(index),
            static_cast<std::uint32_t>(block), topology_id_);
        const auto begin = std::max(first_byte, block * block_payload_bytes) -
                           block * block_payload_bytes;
        const auto end = std::min(last_byte, (block + 1) * block_payload_bytes) -
                         block * block_payload_bytes;
        for (auto at = begin; at < end; at += descriptor.width)
            result.push_back(descriptor.width == 4 ?
                get_u32(payload.data() + at) :
                get_u64(payload.data() + at));
    }
    if (result.size() != count)
        throw std::runtime_error("region_topology_array_changed");
    return result;
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_connectivity_regions.py@7536139:138-203
void NativeRegionTopologyFile::validate_file() const {
    if (!converged_ || counts_[0] > std::uint64_t{0x100000000ULL} ||
        counts_[0] != counts_[1] || counts_[2] != counts_[0] + 1 ||
        counts_[3] != counts_[4] || counts_[5] == 0 ||
        counts_[6] != counts_[3] ||
        counts_[5] - 1 > std::numeric_limits<std::uint32_t>::max())
        throw std::runtime_error("region_topology_array_shape_invalid");
    std::ifstream input(path_, std::ios::binary);
    if (!input) throw std::runtime_error("region_topology_file_invalid");
    const auto descriptor = [&](Section section) {
        const auto index = static_cast<std::size_t>(section);
        return Descriptor{counts_[index], widths_[index], blocks_[index],
                          starts_[index]};
    };
    visit_values(input, descriptor(Section::terms),
                 static_cast<std::uint32_t>(Section::terms), topology_id_,
                 [](std::uint64_t, std::uint64_t) {});
    const auto regions = counts_[5] - 1;
    visit_values(input, descriptor(Section::core_labels),
                 static_cast<std::uint32_t>(Section::core_labels), topology_id_,
                 [&](std::uint64_t value, std::uint64_t) {
        if (value >= regions)
            throw std::runtime_error("region_topology_core_invalid");
    });
    const auto check_offsets = [&](Section section, std::uint64_t expected) {
        std::uint64_t previous = 0;
        bool first_value = true;
        visit_values(input, descriptor(section),
                     static_cast<std::uint32_t>(section), topology_id_,
                     [&](std::uint64_t value, std::uint64_t) {
            if ((first_value && value != 0) || value < previous ||
                value > expected)
                throw std::runtime_error("region_topology_offset_invalid");
            first_value = false;
            previous = value;
        });
        if (previous != expected)
            throw std::runtime_error("region_topology_offset_invalid");
    };
    check_offsets(Section::member_offsets, counts_[3]);
    visit_values(input, descriptor(Section::member_regions),
                 static_cast<std::uint32_t>(Section::member_regions),
                 topology_id_, [&](std::uint64_t value, std::uint64_t) {
        if (value >= regions)
            throw std::runtime_error("region_topology_membership_invalid");
    });
    visit_values(input, descriptor(Section::member_weights),
                 static_cast<std::uint32_t>(Section::member_weights),
                 topology_id_, [&](std::uint64_t bits, std::uint64_t) {
        const auto value = std::bit_cast<double>(bits);
        if (!std::isfinite(value) || value <= 0)
            throw std::runtime_error("region_topology_membership_invalid");
    });
    check_offsets(Section::region_offsets, counts_[6]);
    visit_values(input, descriptor(Section::region_nodes),
                 static_cast<std::uint32_t>(Section::region_nodes), topology_id_,
                 [&](std::uint64_t value, std::uint64_t) {
        if (value >= counts_[0])
            throw std::runtime_error("region_topology_node_invalid");
    });
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_connectivity_regions.py@7536139:138-157
std::uint64_t NativeRegionTopologyFile::term_count() const {
    return counts_[static_cast<std::size_t>(Section::terms)];
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_connectivity_regions.py@7536139:138-157
std::uint32_t NativeRegionTopologyFile::term(std::uint32_t local) const {
    return static_cast<std::uint32_t>(
        read_values(Section::terms, local, 1).front());
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_connectivity_regions.py@7536139:138-157
std::uint32_t NativeRegionTopologyFile::core_label(
    std::uint32_t local) const {
    return static_cast<std::uint32_t>(
        read_values(Section::core_labels, local, 1).front());
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_region_arrays.py@7536139:29-36
std::uint64_t NativeRegionTopologyFile::region_count() const {
    return counts_[static_cast<std::size_t>(Section::region_offsets)] - 1;
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_region_arrays.py@7536139:29-36
std::uint64_t NativeRegionTopologyFile::region_size(
    std::uint32_t region) const {
    if (region >= region_count())
        throw std::out_of_range("region outside directory");
    const auto offsets = read_values(Section::region_offsets, region, 2);
    return offsets[1] - offsets[0];
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_region_arrays.py@7536139:29-36
std::uint32_t NativeRegionTopologyFile::region_node(
    std::uint32_t region, std::uint64_t offset) const {
    if (region >= region_count())
        throw std::out_of_range("region outside directory");
    const auto bounds = read_values(Section::region_offsets, region, 2);
    if (offset >= bounds[1] - bounds[0])
        throw std::out_of_range("region node outside directory");
    return static_cast<std::uint32_t>(
        read_values(Section::region_nodes, bounds[0] + offset, 1).front());
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_connectivity_regions.py@7536139:205-211
std::vector<std::pair<std::uint32_t, double>>
NativeRegionTopologyFile::memberships_for_term(
    std::uint32_t local_node) const {
    if (local_node >= term_count())
        throw std::out_of_range("region node outside directory");
    const auto bounds = read_values(Section::member_offsets, local_node, 2);
    const auto count = bounds[1] - bounds[0];
    const auto regions = read_values(Section::member_regions, bounds[0], count);
    const auto weights = read_values(Section::member_weights, bounds[0], count);
    std::vector<std::pair<std::uint32_t, double>> result;
    result.reserve(static_cast<std::size_t>(count));
    for (std::size_t at = 0; at < regions.size(); ++at)
        result.emplace_back(static_cast<std::uint32_t>(regions[at]),
                            std::bit_cast<double>(weights[at]));
    return result;
}

}  // namespace swegca::vrs
