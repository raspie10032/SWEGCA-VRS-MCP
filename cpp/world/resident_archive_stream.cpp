#include "world/resident_archive_stream.hpp"

#include "swegca_architecture/sha256.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

namespace swegca::world {
namespace {

constexpr std::size_t stream_chunk_bytes = 4U * 1024U * 1024U;

std::string hex(const architecture::DigestBytes& digest) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result(digest.size() * 2, '0');
    for (std::size_t i = 0; i < digest.size(); ++i) {
        const auto value = std::to_integer<unsigned>(digest[i]);
        result[2 * i] = digits[value >> 4U]; result[2 * i + 1] = digits[value & 15U];
    }
    return result;
}

void rewind(std::istream& stream) {
    stream.clear(); stream.seekg(0, std::ios::beg);
    if (!stream) throw std::invalid_argument("resident stream is not seekable");
}

void read_exact(std::istream& stream, std::span<std::byte> destination) {
    std::size_t position{};
    while (position < destination.size()) {
        const auto count = std::min<std::size_t>(destination.size() - position,
            static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max()));
        stream.read(reinterpret_cast<char*>(destination.data() + position),
                    static_cast<std::streamsize>(count));
        const auto received = stream.gcount();
        if (received <= 0) throw std::invalid_argument("truncated resident stream");
        position += static_cast<std::size_t>(received);
    }
}

void require_eof(std::istream& stream) {
    char extra{};
    stream.read(&extra, 1);
    if (stream.gcount() != 0) throw std::invalid_argument("unclaimed resident stream bytes");
    if (!stream.eof()) throw std::invalid_argument("invalid resident stream read");
}

std::string hash_stream(std::istream& stream, const std::size_t expected_bytes) {
    rewind(stream);
    architecture::Sha256 digest;
    std::vector<std::byte> buffer(std::min(stream_chunk_bytes, expected_bytes));
    std::size_t remaining = expected_bytes;
    while (remaining) {
        const auto count = std::min(buffer.size(), remaining);
        read_exact(stream, std::span(buffer).first(count));
        digest.update(std::span(buffer).first(count)); remaining -= count;
    }
    require_eof(stream);
    return hex(digest.finish());
}

}  // namespace

ResidentArchiveWriteReceipt write_resident_directory(
    const CompressedMemoryActivationIndex& source,
    std::ostream& stream, const std::size_t maximum_bytes) {
    const auto archive = dump_directory_leaf(source);
    if (archive.size() > maximum_bytes)
        throw std::invalid_argument("resident export exceeds declared capacity");
    std::size_t position{};
    while (position < archive.size()) {
        const auto count = std::min(stream_chunk_bytes, archive.size() - position);
        stream.write(reinterpret_cast<const char*>(archive.data() + position),
                     static_cast<std::streamsize>(count));
        if (!stream) throw std::runtime_error("resident export write failed");
        position += count;
    }
    return {archive.size(), resident_archive_sha256(archive)};
}

std::shared_ptr<const ResidentDirectoryMemoryIndex> load_resident_stream(
    std::istream& stream, const std::size_t expected_bytes,
    const std::string_view expected_sha256,
    const std::string_view expected_snapshot_id,
    const std::size_t maximum_bytes) {
    if (expected_bytes < native_resident_directory_magic.size() + sizeof(std::uint32_t) ||
        expected_bytes > maximum_bytes)
        throw std::invalid_argument("resident stream exceeds cold capacity");
    if (hash_stream(stream, expected_bytes) != expected_sha256)
        throw std::invalid_argument("resident directory seal differs");
    rewind(stream);
    std::vector<std::byte> archive(expected_bytes);
    read_exact(stream, archive); require_eof(stream);
    if (resident_archive_sha256(archive) != expected_sha256)
        throw std::invalid_argument("resident stream changed between seal passes");
    return load_directory_leaf(archive, expected_sha256,
                               expected_snapshot_id, maximum_bytes);
}

}  // namespace swegca::world
