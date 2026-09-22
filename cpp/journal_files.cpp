#include "journal_files.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace swegca::vrs {
namespace {

constexpr std::size_t magic_bytes = 8;
constexpr std::size_t header_bytes = 8;
constexpr std::size_t checksum_bytes = 32;
constexpr std::uint64_t maximum_payload_bytes = 64ULL * 1024 * 1024;
constexpr std::array<char, magic_bytes> file_magic{'V','R','S','2','J','N','L','1'};

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:147-162
void durable_truncate(const std::filesystem::path& path, std::uint64_t length) {
#if defined(_WIN32)
    const auto descriptor = _wopen(path.c_str(), _O_BINARY | _O_RDWR);
    if (descriptor < 0) throw std::runtime_error("native_vrs_journal_truncated");
    const auto truncated = _chsize_s(descriptor, length);
    const auto synced = truncated == 0 ? _commit(descriptor) : -1;
    _close(descriptor);
#else
    const auto descriptor = open(path.c_str(), O_RDWR);
    if (descriptor < 0) throw std::runtime_error("native_vrs_journal_truncated");
    const auto truncated = ftruncate(descriptor, static_cast<off_t>(length));
    const auto synced = truncated == 0 ? fsync(descriptor) : -1;
    close(descriptor);
#endif
    if (truncated != 0 || synced != 0)
        throw std::runtime_error("native_vrs_journal_truncated");
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:153-155
std::uint64_t frame_length(const std::array<char, header_bytes>& header) {
    std::uint64_t length = 0;
    for (std::size_t i = 0; i < header.size(); ++i)
        length |= static_cast<std::uint64_t>(static_cast<unsigned char>(header[i])) << (8 * i);
    return length;
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:137-177
void visit_file(const std::filesystem::path& path, bool repair,
                const std::function<void(JournalRow&&)>& visit) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("native_vrs_journal_magic_invalid");
    std::array<char, magic_bytes> magic{};
    stream.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    if (stream.bad()) throw std::runtime_error("native_vrs_journal_read_failed");
    if (stream.gcount() != static_cast<std::streamsize>(magic.size()) || magic != file_magic)
        throw std::runtime_error("native_vrs_journal_magic_invalid");
    std::uint64_t valid_end = magic.size();
    while (true) {
        std::array<char, header_bytes> header{};
        stream.read(header.data(), static_cast<std::streamsize>(header.size()));
        if (stream.bad()) throw std::runtime_error("native_vrs_journal_read_failed");
        const auto count = stream.gcount();
        if (count == 0 && stream.eof()) break;
        if (count != static_cast<std::streamsize>(header.size())) {
            if (!repair) throw std::runtime_error("native_vrs_journal_truncated");
            stream.close();
            durable_truncate(path, valid_end);
            break;
        }
        const auto length = frame_length(header);
        if (length > maximum_payload_bytes)
            throw std::runtime_error("native_vrs_frame_too_large");
        std::vector<std::byte> frame(header_bytes + length + checksum_bytes);
        for (std::size_t i = 0; i < header_bytes; ++i)
            frame[i] = static_cast<std::byte>(static_cast<unsigned char>(header[i]));
        const auto remaining = static_cast<std::streamsize>(length + checksum_bytes);
        stream.read(reinterpret_cast<char*>(frame.data() + header_bytes), remaining);
        if (stream.bad()) throw std::runtime_error("native_vrs_journal_read_failed");
        if (stream.gcount() != remaining) {
            if (!repair) throw std::runtime_error("native_vrs_journal_truncated");
            stream.close();
            durable_truncate(path, valid_end);
            break;
        }
        visit_journal_frame(frame, visit);
        valid_end += frame.size();
    }
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:179-181
std::vector<std::filesystem::path> ordered_files(const std::filesystem::path& directory) {
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        const auto name = entry.path().filename().string();
        if (name.starts_with("segment-") && name.ends_with(".vrsj"))
            files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    files.push_back(directory / "head.vrsj");
    return files;
}

}  // namespace

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:183-194
JournalScan visit_journal_files(const std::filesystem::path& generation_directory,
                                bool repair_head_tail,
                                const std::function<void(JournalRow&&)>& visit) {
    JournalScan scan;
    std::int64_t expected = 1;
    const auto files = ordered_files(generation_directory);
    for (std::size_t i = 0; i < files.size(); ++i) {
        const bool repair = repair_head_tail && i + 1 == files.size();
        visit_file(files[i], repair, [&](JournalRow&& row) {
            if (row.sequence != expected)
                throw std::runtime_error("native_vrs_sequence_invalid");
            ++expected;
            ++scan.row_count;
            scan.last_sequence = row.sequence;
            scan.last_pair = row.pair_id;
            visit(std::move(row));
        });
    }
    return scan;
}

}  // namespace swegca::vrs
