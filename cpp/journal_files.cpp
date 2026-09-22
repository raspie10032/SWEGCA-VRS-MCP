#include "journal_files.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <limits>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
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
constexpr std::uint64_t rotate_bytes = 32ULL * 1024 * 1024;
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

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:39-46
void fsync_directory(const std::filesystem::path& directory) {
#if !defined(_WIN32)
    const auto descriptor = open(directory.c_str(), O_RDONLY | O_DIRECTORY);
    if (descriptor < 0) throw std::runtime_error("native_vrs_directory_sync_failed");
    const auto synced = fsync(descriptor);
    close(descriptor);
    if (synced != 0) throw std::runtime_error("native_vrs_directory_sync_failed");
#else
    (void)directory;
#endif
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:49-62
void write_all(int descriptor, std::span<const std::byte> bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
#if defined(_WIN32)
        const auto chunk = static_cast<unsigned int>(std::min<std::size_t>(
            bytes.size() - offset, std::numeric_limits<unsigned int>::max()));
        const auto count = _write(descriptor, bytes.data() + offset, chunk);
#else
        const auto count = ::write(descriptor, bytes.data() + offset, bytes.size() - offset);
#endif
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) throw std::runtime_error("native_vrs_journal_write_failed");
        offset += static_cast<std::size_t>(count);
    }
}

class NativeDescriptor {
public:
    // SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:49-62
    explicit NativeDescriptor(int descriptor) : descriptor_(descriptor) {}
    NativeDescriptor(const NativeDescriptor&) = delete;
    NativeDescriptor& operator=(const NativeDescriptor&) = delete;

    // SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:49-62
    ~NativeDescriptor() {
        if (descriptor_ >= 0) {
#if defined(_WIN32)
            _close(descriptor_);
#else
            close(descriptor_);
#endif
        }
    }

    // SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:49-62
    [[nodiscard]] int get() const { return descriptor_; }

    // SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:49-62
    void sync() {
#if defined(_WIN32)
        const auto result = _commit(descriptor_);
#else
        const auto result = fsync(descriptor_);
#endif
        if (result != 0) throw std::runtime_error("native_vrs_journal_write_failed");
    }

    // SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:49-62
    void close_now() {
        const auto previous = std::exchange(descriptor_, -1);
#if defined(_WIN32)
        const auto result = _close(previous);
#else
        const auto result = close(previous);
#endif
        if (result != 0) throw std::runtime_error("native_vrs_journal_write_failed");
    }

private:
    int descriptor_;
};

}  // namespace

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:49-62
void write_atomic_file(const std::filesystem::path& path,
                       std::span<const std::byte> bytes) {
    if (std::filesystem::create_directories(path.parent_path()))
        std::filesystem::permissions(path.parent_path(),
            std::filesystem::perms::owner_all,
            std::filesystem::perm_options::replace);
    std::random_device random;
    const auto temporary = path.parent_path() /
        ("." + path.filename().string() + "-" + std::to_string(random()) +
         "-" + std::to_string(random()));
#if defined(_WIN32)
    const auto raw_descriptor = _wopen(temporary.c_str(), _O_BINARY | _O_WRONLY | _O_CREAT | _O_EXCL,
                                   _S_IREAD | _S_IWRITE);
#else
    const auto raw_descriptor = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
#endif
    if (raw_descriptor < 0) throw std::runtime_error("native_vrs_head_create_failed");
    NativeDescriptor descriptor(raw_descriptor);
    try {
        write_all(descriptor.get(), bytes);
        descriptor.sync();
        descriptor.close_now();
        std::filesystem::rename(temporary, path);
        fsync_directory(path.parent_path());
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}

namespace {

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:49-62
void atomic_head_magic(const std::filesystem::path& path) {
    write_atomic_file(path, std::as_bytes(std::span(file_magic)));
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:235-242
void durable_append(const std::filesystem::path& path, std::span<const std::byte> bytes) {
#if defined(_WIN32)
    const auto raw_descriptor = _wopen(path.c_str(), _O_BINARY | _O_WRONLY | _O_APPEND);
#else
    const auto raw_descriptor = open(path.c_str(), O_WRONLY | O_APPEND);
#endif
    if (raw_descriptor < 0) throw std::runtime_error("native_vrs_journal_write_failed");
    NativeDescriptor descriptor(raw_descriptor);
    write_all(descriptor.get(), bytes);
    descriptor.sync();
    descriptor.close_now();
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:249-257
void rotate_head(const std::filesystem::path& directory, std::int64_t last_sequence) {
    const auto head = directory / "head.vrsj";
    if (std::filesystem::file_size(head) <= magic_bytes) return;
    std::ostringstream name;
    name << "segment-" << std::setw(20) << std::setfill('0') << last_sequence << ".vrsj";
    std::filesystem::rename(head, directory / name.str());
    atomic_head_magic(head);
    fsync_directory(directory);
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:153-155
std::uint64_t frame_length(const std::array<char, header_bytes>& header) {
    std::uint64_t length = 0;
    for (std::size_t i = 0; i < header.size(); ++i)
        length |= static_cast<std::uint64_t>(static_cast<unsigned char>(header[i])) << (8 * i);
    return length;
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:137-177
bool visit_file(const std::filesystem::path& path, bool repair,
                const std::function<bool(JournalRow&&)>& visit) {
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
        if (!visit_journal_frame_until(frame, visit)) return false;
        valid_end += frame.size();
    }
    return true;
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
        (void)visit_file(files[i], repair, [&](JournalRow&& row) {
            if (row.sequence != expected)
                throw std::runtime_error("native_vrs_sequence_invalid");
            ++expected;
            ++scan.row_count;
            scan.last_sequence = row.sequence;
            scan.last_pair = row.pair_id;
            visit(std::move(row));
            return true;
        });
    }
    return scan;
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:179-221
void visit_journal_rows_until(
    const std::filesystem::path& generation_directory,
    const std::function<bool(JournalRow&&)>& visit) {
    std::int64_t expected = 1;
    for (const auto& file : ordered_files(generation_directory)) {
        const bool completed = visit_file(file, false, [&](JournalRow&& row) {
            if (row.sequence != expected)
                throw std::runtime_error("native_vrs_sequence_invalid");
            ++expected;
            return visit(std::move(row));
        });
        if (!completed) return;
    }
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:223-257
std::vector<std::int64_t> append_journal_rows(
    const std::filesystem::path& generation_directory,
    JournalScan& current,
    std::span<const PendingJournalRow> rows) {
    if (rows.empty()) return {};
    if (current.last_sequence < 0 || rows.size() > static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max() - current.last_sequence))
        throw std::runtime_error("native_vrs_sequence_invalid");
    const auto first = current.last_sequence + 1;
    if (current.row_count > std::numeric_limits<std::uint64_t>::max() - rows.size())
        throw std::runtime_error("native_vrs_sequence_invalid");
    std::vector<std::int64_t> sequences;
    sequences.reserve(rows.size());
    for (std::size_t i = 0; i < rows.size(); ++i)
        sequences.push_back(first + static_cast<std::int64_t>(i));
    auto last_pair = rows.back().pair_id;
    const auto frame = encode_journal_frame(first, rows);
    const auto head = generation_directory / "head.vrsj";
    const auto start = std::filesystem::file_size(head);
    try {
        durable_append(head, frame);
    } catch (...) {
        durable_truncate(head, start);
        throw;
    }
    current.row_count += rows.size();
    current.last_sequence = sequences.back();
    current.last_pair = std::move(last_pair);
    if (std::filesystem::file_size(head) >= rotate_bytes)
        rotate_head(generation_directory, current.last_sequence);
    return sequences;
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:295-312
void write_generation_head(
    const std::filesystem::path& generation_directory,
    const JournalRowProducer& produce_rows) {
    const auto path = generation_directory / "head.vrsj";
#if defined(_WIN32)
    const auto raw_descriptor = _wopen(path.c_str(),
        _O_BINARY | _O_WRONLY | _O_CREAT | _O_EXCL,
        _S_IREAD | _S_IWRITE);
#else
    const auto raw_descriptor = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
#endif
    if (raw_descriptor < 0) throw std::runtime_error("native_vrs_head_create_failed");
    NativeDescriptor descriptor(raw_descriptor);
    write_all(descriptor.get(), std::as_bytes(std::span(file_magic)));
    std::vector<JournalRow> batch;
    batch.reserve(512);
    std::uint64_t expected = 1;
    produce_rows([&](JournalRow&& row) {
        if (row.sequence < 1 || static_cast<std::uint64_t>(row.sequence) != expected)
            throw std::runtime_error("native_vrs_sequence_invalid");
        ++expected;
        batch.push_back(std::move(row));
        if (batch.size() == 512) {
            const auto frame = encode_journal_frame(batch);
            write_all(descriptor.get(), frame);
            batch.clear();
        }
    });
    if (!batch.empty()) {
        const auto frame = encode_journal_frame(batch);
        write_all(descriptor.get(), frame);
    }
    descriptor.sync();
    descriptor.close_now();
    fsync_directory(generation_directory);
    fsync_directory(generation_directory.parent_path());
}

}  // namespace swegca::vrs
