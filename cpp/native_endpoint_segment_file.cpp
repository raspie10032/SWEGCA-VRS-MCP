#include "native_endpoint_segment_file.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
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

constexpr std::string_view magic = "VRS2EPS1";
constexpr std::uint64_t header_bytes = 96;
constexpr std::uint64_t entry_bytes = 12;
constexpr std::uint64_t max_edge_address =
    std::numeric_limits<std::uint32_t>::max();

struct Entry {
    std::uint32_t node;
    std::uint32_t edge;
};

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_dependency_index.py@7536139:13-18
bool valid_extent(std::uint64_t first, std::uint64_t count) {
    return count != 0 && count <= max_edge_address &&
           first <= max_edge_address - count;
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:26-29
void put_u32(unsigned char* out, std::uint32_t value) {
    for (unsigned at = 0; at < 4; ++at)
        out[at] = static_cast<unsigned char>(value >> (8 * at));
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:26-29
std::uint32_t get_u32(const unsigned char* in) {
    std::uint32_t value = 0;
    for (unsigned at = 0; at < 4; ++at)
        value |= std::uint32_t(in[at]) << (8 * at);
    return value;
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:26-29
void put_u64(unsigned char* out, std::uint64_t value) {
    for (unsigned at = 0; at < 8; ++at)
        out[at] = static_cast<unsigned char>(value >> (8 * at));
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:26-29
std::uint64_t get_u64(const unsigned char* in) {
    std::uint64_t value = 0;
    for (unsigned at = 0; at < 8; ++at)
        value |= std::uint64_t(in[at]) << (8 * at);
    return value;
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:49-62
void sync_path(const std::filesystem::path& path, bool directory) {
#if defined(_WIN32)
    if (directory) return;
    const auto descriptor = _wopen(path.c_str(), _O_BINARY | _O_RDONLY);
    if (descriptor < 0) throw std::runtime_error("endpoint_segment_sync_failed");
    const auto result = _commit(descriptor);
    _close(descriptor);
#else
    const auto descriptor = open(path.c_str(), O_RDONLY);
    if (descriptor < 0) throw std::runtime_error("endpoint_segment_sync_failed");
    const auto result = fsync(descriptor);
    close(descriptor);
#endif
    if (result != 0) throw std::runtime_error("endpoint_segment_sync_failed");
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:91-127
bool valid_generation(std::string_view generation) {
    return generation.size() == 34 && generation.starts_with("g-") &&
        std::all_of(generation.begin() + 2, generation.end(), [](char digit) {
            return (digit >= '0' && digit <= '9') ||
                   (digit >= 'a' && digit <= 'f');
        });
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_dependency_index.py@7536139:13-18
std::array<unsigned char, header_bytes> make_header(
    const NativeEndpointSegment& segment) {
    if (!valid_generation(segment.journal_generation) ||
        !valid_extent(segment.first_edge, segment.edge_count))
        throw std::runtime_error("endpoint_segment_metadata_invalid");
    std::array<unsigned char, header_bytes> header{};
    std::copy(magic.begin(), magic.end(), header.begin());
    std::copy(segment.journal_generation.begin(),
              segment.journal_generation.end(), header.begin() + 8);
    put_u64(header.data() + 48, segment.first_edge);
    put_u64(header.data() + 56, segment.edge_count);
    put_u32(header.data() + 64,
            crc32(0, reinterpret_cast<const Bytef*>(header.data()), 64));
    return header;
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_dependency_index.py@7536139:13-18
void require_header(std::ifstream& stream,
                    const NativeEndpointSegment& segment) {
    if (segment.path.parent_path().empty() ||
        segment.path.filename().string().rfind("eps-", 0) != 0 ||
        segment.path.extension() != ".vrs" ||
        !std::filesystem::is_regular_file(segment.path) ||
        std::filesystem::is_symlink(segment.path) ||
        std::filesystem::file_size(segment.path) !=
            header_bytes + 2 * segment.edge_count * entry_bytes)
        throw std::runtime_error("endpoint_segment_file_invalid");
    std::array<unsigned char, header_bytes> actual{};
    stream.seekg(0);
    stream.read(reinterpret_cast<char*>(actual.data()), actual.size());
    if (stream.gcount() != static_cast<std::streamsize>(actual.size()) ||
        actual != make_header(segment))
        throw std::runtime_error("endpoint_segment_header_invalid");
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_dependency_index.py@7536139:13-18
void write_entry(std::ostream& output, Entry entry) {
    std::array<unsigned char, entry_bytes> bytes{};
    put_u32(bytes.data(), entry.node);
    put_u32(bytes.data() + 4, entry.edge);
    put_u32(bytes.data() + 8,
            crc32(0, reinterpret_cast<const Bytef*>(bytes.data()), 8));
    output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (!output) throw std::runtime_error("endpoint_segment_write_failed");
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_dependency_index.py@7536139:55-68
Entry read_entry(std::ifstream& input,
                 const NativeEndpointSegment& segment,
                 EndpointDirection direction,
                 std::uint64_t index) {
    if (index >= segment.edge_count)
        throw std::runtime_error("endpoint_segment_address_invalid");
    const auto base = header_bytes +
        (direction == EndpointDirection::incoming ?
             segment.edge_count * entry_bytes : 0);
    input.clear();
    input.seekg(static_cast<std::streamoff>(base + index * entry_bytes));
    std::array<unsigned char, entry_bytes> bytes{};
    input.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
    if (input.gcount() != static_cast<std::streamsize>(bytes.size()) ||
        get_u32(bytes.data() + 8) !=
            crc32(0, reinterpret_cast<const Bytef*>(bytes.data()), 8))
        throw std::runtime_error("endpoint_segment_entry_corrupt");
    const auto edge = get_u32(bytes.data() + 4);
    if (edge < segment.first_edge ||
        std::uint64_t(edge) >= segment.first_edge + segment.edge_count)
        throw std::runtime_error("endpoint_segment_entry_address_invalid");
    return Entry{get_u32(bytes.data()), edge};
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_dependency_index.py@7536139:13-18
std::filesystem::path fresh_path(
    const std::filesystem::path& directory,
    std::uint64_t first, std::uint64_t count) {
    std::random_device random;
    constexpr char digits[] = "0123456789abcdef";
    for (unsigned attempt = 0; attempt < 16; ++attempt) {
        std::string suffix;
        suffix.reserve(32);
        for (unsigned byte = 0; byte < 16; ++byte) {
            const auto value = static_cast<unsigned char>(random());
            suffix.push_back(digits[value >> 4]);
            suffix.push_back(digits[value & 15]);
        }
        const auto path = directory /
            ("eps-" + std::to_string(first) + "-" +
             std::to_string(count) + "-" + suffix + ".vrs");
        if (!std::filesystem::exists(path)) return path;
    }
    throw std::runtime_error("endpoint_segment_name_exhausted");
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:49-62
void require_writer_directory(const std::filesystem::path& directory,
                              OwnerLock& owner) {
    if (!owner.locked())
        throw std::runtime_error("native_vrs_owner_lock_required");
    if (std::filesystem::create_directories(directory))
        std::filesystem::permissions(
            directory, std::filesystem::perms::owner_all,
            std::filesystem::perm_options::replace);
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:49-62
template <typename Fill>
NativeEndpointSegment write_segment(
    const std::filesystem::path& directory,
    std::string generation, std::uint64_t first, std::uint64_t count,
    OwnerLock& owner, Fill&& fill) {
    require_writer_directory(directory, owner);
    NativeEndpointSegment segment{
        fresh_path(directory, first, count), std::move(generation), first, count};
    const auto header = make_header(segment);
    try {
        std::ofstream output(segment.path, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("endpoint_segment_write_failed");
        output.write(reinterpret_cast<const char*>(header.data()), header.size());
        if (!output) throw std::runtime_error("endpoint_segment_write_failed");
        fill(output);
        output.flush();
        if (!output) throw std::runtime_error("endpoint_segment_write_failed");
        output.close();
        if (!output) throw std::runtime_error("endpoint_segment_write_failed");
        if (std::filesystem::file_size(segment.path) !=
            header_bytes + 2 * count * entry_bytes)
            throw std::runtime_error("endpoint_segment_write_failed");
        sync_path(segment.path, false);
        sync_path(directory, true);
        return segment;
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(segment.path, ignored);
        throw;
    }
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_dependency_index.py@7536139:21-27
class Cursor {
public:
    Cursor(const NativeEndpointSegment& segment, EndpointDirection direction)
        : segment_(segment), direction_(direction), input_(segment.path, std::ios::binary) {
        if (!input_) throw std::runtime_error("endpoint_segment_file_invalid");
        require_header(input_, segment_);
        advance();
    }

    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_dependency_index.py@7536139:21-27
    [[nodiscard]] bool active() const { return current_.has_value(); }
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_dependency_index.py@7536139:21-27
    [[nodiscard]] Entry current() const { return *current_; }

    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_dependency_index.py@7536139:21-27
    void advance() {
        if (next_ == segment_.edge_count) {
            current_.reset();
            return;
        }
        auto value = read_entry(input_, segment_, direction_, next_++);
        if (previous_ &&
            (value.node < previous_->node ||
             (value.node == previous_->node && value.edge <= previous_->edge)))
            throw std::runtime_error("endpoint_segment_order_changed");
        previous_ = value;
        current_ = value;
    }

private:
    NativeEndpointSegment segment_;
    EndpointDirection direction_;
    std::ifstream input_;
    std::uint64_t next_ = 0;
    std::optional<Entry> previous_;
    std::optional<Entry> current_;
};

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_dependency_index.py@7536139:21-27
void merge_direction(std::ostream& output,
                     const NativeEndpointSegment& left,
                     const NativeEndpointSegment& right,
                     EndpointDirection direction) {
    Cursor a(left, direction), b(right, direction);
    while (a.active() || b.active()) {
        const bool take_left = a.active() &&
            (!b.active() || a.current().node <= b.current().node);
        if (take_left) {
            write_entry(output, a.current());
            a.advance();
        } else {
            write_entry(output, b.current());
            b.advance();
        }
    }
}

}  // namespace

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_dependency_index.py@7536139:13-18
NativeEndpointSegment NativeEndpointSegmentFile::create(
    const std::filesystem::path& directory, std::string journal_generation,
    std::uint64_t first_edge, std::span<const EventEdge> appended,
    OwnerLock& owner) {
    if (!valid_extent(first_edge, appended.size()))
        throw std::runtime_error("endpoint_segment_append_invalid");
    std::vector<Entry> outgoing, incoming;
    outgoing.reserve(appended.size());
    incoming.reserve(appended.size());
    for (std::size_t at = 0; at < appended.size(); ++at) {
        const auto address = static_cast<std::uint32_t>(first_edge + at);
        outgoing.push_back(Entry{appended[at].source, address});
        incoming.push_back(Entry{appended[at].target, address});
    }
    const auto by_node = [](const Entry& a, const Entry& b) {
        return a.node < b.node;
    };
    std::stable_sort(outgoing.begin(), outgoing.end(), by_node);
    std::stable_sort(incoming.begin(), incoming.end(), by_node);
    return write_segment(directory, std::move(journal_generation), first_edge,
                         appended.size(), owner, [&](std::ostream& output) {
        for (const auto entry : outgoing) write_entry(output, entry);
        for (const auto entry : incoming) write_entry(output, entry);
    });
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_dependency_index.py@7536139:21-27
NativeEndpointSegment NativeEndpointSegmentFile::merge(
    const std::filesystem::path& directory,
    const NativeEndpointSegment& left,
    const NativeEndpointSegment& right,
    OwnerLock& owner) {
    if (!valid_extent(left.first_edge, left.edge_count) ||
        !valid_extent(right.first_edge, right.edge_count) ||
        left.journal_generation != right.journal_generation ||
        left.path.parent_path() != directory ||
        right.path.parent_path() != directory ||
        left.edge_count == 0 || right.edge_count == 0 ||
        left.first_edge + left.edge_count != right.first_edge ||
        left.edge_count > max_edge_address - right.edge_count)
        throw std::runtime_error("endpoint_segment_merge_invalid");
    return write_segment(directory, left.journal_generation,
                         left.first_edge,
                         left.edge_count + right.edge_count,
                         owner, [&](std::ostream& output) {
        merge_direction(output, left, right, EndpointDirection::outgoing);
        merge_direction(output, left, right, EndpointDirection::incoming);
    });
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_dependency_index.py@7536139:13-27
void NativeEndpointSegmentFile::validate_all(
    const NativeEndpointSegment& segment) {
    if (!valid_extent(segment.first_edge, segment.edge_count) ||
        segment.edge_count > std::numeric_limits<std::size_t>::max() - 7)
        throw std::runtime_error("endpoint_segment_validation_too_large");
    std::vector<std::uint8_t> seen(
        static_cast<std::size_t>((segment.edge_count + 7) / 8));
    for (const auto direction : {EndpointDirection::outgoing,
                                 EndpointDirection::incoming}) {
        std::fill(seen.begin(), seen.end(), std::uint8_t{0});
        Cursor cursor(segment, direction);
        std::uint64_t count = 0;
        while (cursor.active()) {
            const auto edge = cursor.current().edge - segment.first_edge;
            const auto byte = static_cast<std::size_t>(edge / 8);
            const auto bit = static_cast<std::uint8_t>(1u << (edge % 8));
            if (seen[byte] & bit)
                throw std::runtime_error("endpoint_segment_duplicate_edge");
            seen[byte] |= bit;
            ++count;
            cursor.advance();
        }
        if (count != segment.edge_count)
            throw std::runtime_error("endpoint_segment_missing_edge");
    }
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_dependency_index.py@7536139:55-68
void NativeEndpointSegmentFile::visit_edges(
    const NativeEndpointSegment& segment,
    std::uint32_t node, EndpointDirection direction,
    const std::function<void(std::uint32_t)>& visit) {
    if (!visit) throw std::runtime_error("endpoint_segment_visitor_missing");
    std::ifstream input(segment.path, std::ios::binary);
    if (!input) throw std::runtime_error("endpoint_segment_file_invalid");
    require_header(input, segment);
    std::uint64_t first = 0, last = segment.edge_count;
    while (first < last) {
        const auto middle = first + (last - first) / 2;
        const auto entry = read_entry(input, segment, direction, middle);
        if (entry.node < node) first = middle + 1;
        else last = middle;
    }
    for (auto at = first; at < segment.edge_count; ++at) {
        const auto entry = read_entry(input, segment, direction, at);
        if (entry.node != node) break;
        visit(entry.edge);
    }
}

}  // namespace swegca::vrs
