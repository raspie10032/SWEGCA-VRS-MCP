#include "swegca_vrs/journal_file_io.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#endif

namespace swegca::vrs::journal::io {
namespace {

namespace fs = std::filesystem;

// SWEGCA: user@2026-09-22:72-79
[[noreturn]] void fail(const char* code) { throw std::runtime_error(code); }

#if defined(_WIN32)

// SWEGCA: user@2026-09-22:72-79
[[noreturn]] void fail_last_error(const char* code) {
    throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(), code);
}

// Owns one Windows handle; closing never throws.
class Handle final {
public:
    // SWEGCA: user@2026-09-22:72-79
    explicit Handle(HANDLE value) noexcept : value_(value) {}
    // SWEGCA: user@2026-09-22:72-79
    Handle(Handle&& other) noexcept : value_(std::exchange(other.value_, INVALID_HANDLE_VALUE)) {}
    // SWEGCA: user@2026-09-22:72-79
    ~Handle() {
        if (value_ != INVALID_HANDLE_VALUE) ::CloseHandle(value_);
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    // SWEGCA: user@2026-09-22:72-79
    [[nodiscard]] HANDLE get() const noexcept { return value_; }
    // SWEGCA: user@2026-09-22:72-79
    [[nodiscard]] bool valid() const noexcept { return value_ != INVALID_HANDLE_VALUE; }
    // SWEGCA: user@2026-09-22:72-79
    void close_checked(const char* code) {
        const HANDLE value = std::exchange(value_, INVALID_HANDLE_VALUE);
        if (value != INVALID_HANDLE_VALUE && !::CloseHandle(value)) fail_last_error(code);
    }

private:
    HANDLE value_;
};

// Readers share delete access so HEAD can be replaced while they read.
constexpr DWORD share_all = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

// SWEGCA: user@2026-09-22:72-79
Handle open_file(const fs::path& path, DWORD access, DWORD disposition, const char* missing_code) {
    Handle handle(::CreateFileW(path.c_str(), access, share_all, nullptr, disposition,
                                FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!handle.valid()) {
        const auto error = ::GetLastError();
        if (missing_code != nullptr &&
            (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND))
            fail(missing_code);
        fail_last_error("journal_file_open_failed");
    }
    return handle;
}

// SWEGCA: user@2026-09-22:72-79
std::uint64_t size_of(const Handle& handle) {
    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(handle.get(), &size)) fail_last_error("journal_file_stat_failed");
    return static_cast<std::uint64_t>(size.QuadPart);
}

// SWEGCA: user@2026-09-22:72-79
OVERLAPPED at_offset(std::uint64_t offset) noexcept {
    OVERLAPPED overlapped{};
    overlapped.Offset = static_cast<DWORD>(offset & 0xffffffffu);
    overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32);
    return overlapped;
}

// Positional write in DWORD-sized chunks; a zero-byte write fails.
// SWEGCA: user@2026-09-22:72-79
void write_at(const Handle& handle, std::span<const std::byte> bytes, std::uint64_t offset) {
    std::size_t at = 0;
    while (at < bytes.size()) {
        const auto chunk = static_cast<DWORD>(
            std::min<std::size_t>(bytes.size() - at, std::numeric_limits<DWORD>::max()));
        auto overlapped = at_offset(offset + at);
        DWORD written = 0;
        if (!::WriteFile(handle.get(), bytes.data() + at, chunk, &written, &overlapped))
            fail_last_error("journal_file_write_failed");
        if (written == 0) fail("journal_file_write_failed");
        at += written;
    }
}

// SWEGCA: user@2026-09-22:72-79
void flush(const Handle& handle, const char* code) {
    if (!::FlushFileBuffers(handle.get())) fail_last_error(code);
}

#else

// SWEGCA: user@2026-09-22:72-79
[[noreturn]] void fail_errno(const char* code) {
    throw std::system_error(errno, std::generic_category(), code);
}

// Owns one POSIX descriptor; closing never throws.
class Descriptor final {
public:
    // SWEGCA: user@2026-09-22:72-79
    explicit Descriptor(int value) noexcept : value_(value) {}
    // SWEGCA: user@2026-09-22:72-79
    Descriptor(Descriptor&& other) noexcept : value_(std::exchange(other.value_, -1)) {}
    // SWEGCA: user@2026-09-22:72-79
    ~Descriptor() {
        if (value_ >= 0) ::close(value_);
    }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    // SWEGCA: user@2026-09-22:72-79
    [[nodiscard]] int get() const noexcept { return value_; }
    // SWEGCA: user@2026-09-22:72-79
    void close_checked(const char* code) {
        const int value = std::exchange(value_, -1);
        if (value >= 0 && ::close(value) != 0) fail_errno(code);
    }

private:
    int value_;
};

// SWEGCA: user@2026-09-22:72-79
Descriptor open_file(const fs::path& path, int flags, const char* missing_code) {
    Descriptor descriptor(::open(path.c_str(), flags | O_CLOEXEC, 0600));
    if (descriptor.get() < 0) {
        if (missing_code != nullptr && errno == ENOENT) fail(missing_code);
        fail_errno("journal_file_open_failed");
    }
    return descriptor;
}

// SWEGCA: user@2026-09-22:72-79
std::uint64_t size_of(const Descriptor& descriptor) {
    struct stat status {};
    if (::fstat(descriptor.get(), &status) != 0) fail_errno("journal_file_stat_failed");
    if (!S_ISREG(status.st_mode)) fail("journal_file_not_regular");
    return static_cast<std::uint64_t>(status.st_size);
}

// Positional write; a zero-byte write fails rather than looping (codex J6).
// SWEGCA: user@2026-09-22:72-79
void write_at(const Descriptor& descriptor, std::span<const std::byte> bytes,
              std::uint64_t offset) {
    std::size_t at = 0;
    while (at < bytes.size()) {
        const auto written = ::pwrite(descriptor.get(), bytes.data() + at, bytes.size() - at,
                                      static_cast<off_t>(offset + at));
        if (written < 0) {
            if (errno == EINTR) continue;
            fail_errno("journal_file_write_failed");
        }
        if (written == 0) fail("journal_file_write_failed");
        at += static_cast<std::size_t>(written);
    }
}

// SWEGCA: user@2026-09-22:72-79
void flush(const Descriptor& descriptor, const char* code) {
    if (::fsync(descriptor.get()) != 0) fail_errno(code);
}

#endif

}  // namespace

#if defined(_WIN32)

// SWEGCA: user@2026-09-22:72-79
OwnerLock::OwnerLock(const fs::path& directory) {
    const auto path = directory / "owner.lock";
    HANDLE handle = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) fail_last_error("journal_owner_lock_open_failed");
    OVERLAPPED whole{};
    if (!::LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, MAXDWORD,
                      MAXDWORD, &whole)) {
        const auto error = ::GetLastError();
        ::CloseHandle(handle);
        if (error == ERROR_LOCK_VIOLATION) fail("journal_already_owned");
        ::SetLastError(error);
        fail_last_error("journal_owner_lock_failed");
    }
    handle_ = reinterpret_cast<std::intptr_t>(handle);
}

// SWEGCA: user@2026-09-22:72-79
OwnerLock::~OwnerLock() {
    if (handle_ != -1) ::CloseHandle(reinterpret_cast<HANDLE>(handle_));
}

// SWEGCA: user@2026-09-22:72-79
void read_range(const fs::path& path, std::uint64_t published, std::uint64_t offset,
                std::span<std::byte> out, const char* missing_code) {
    if (offset > published || out.size() > published - offset) fail("journal_range_invalid");
    auto handle = open_file(path, GENERIC_READ, OPEN_EXISTING, missing_code);
    if (size_of(handle) < published) fail("journal_published_file_truncated");
    std::size_t at = 0;
    while (at < out.size()) {
        const auto chunk = static_cast<DWORD>(
            std::min<std::size_t>(out.size() - at, std::numeric_limits<DWORD>::max()));
        auto overlapped = at_offset(offset + at);
        DWORD read = 0;
        if (!::ReadFile(handle.get(), out.data() + at, chunk, &read, &overlapped))
            fail_last_error("journal_file_read_failed");
        if (read == 0) fail("journal_file_truncated_while_reading");
        at += read;
    }
}

// SWEGCA: user@2026-09-22:72-79
void read_exact_file(const fs::path& path, std::span<std::byte> out, const char* missing_code) {
    {
        auto handle = open_file(path, GENERIC_READ, OPEN_EXISTING, missing_code);
        if (size_of(handle) != out.size()) fail("journal_file_length_invalid");
    }
    read_range(path, out.size(), 0, out, missing_code);
}

// SWEGCA: user@2026-09-22:72-79
void publish_file_parts(const fs::path& path, std::span<const std::span<const std::byte>> parts) {
    fs::path part = path;
    part += part_suffix;
    {
        auto handle = open_file(part, GENERIC_WRITE, CREATE_ALWAYS, nullptr);
        std::uint64_t at = 0;
        for (const auto piece : parts) {
            write_at(handle, piece, at);
            at += piece.size();
        }
        flush(handle, "journal_file_fsync_failed");
        handle.close_checked("journal_file_close_failed");
    }
    if (!::MoveFileExW(part.c_str(), path.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        fail_last_error("journal_file_publish_failed");
}

// SWEGCA: user@2026-09-22:72-79
void append_parts_at_published_end(const fs::path& path, std::uint64_t published,
                                   std::span<const std::span<const std::byte>> parts) {
    auto handle = open_file(path, GENERIC_WRITE, OPEN_EXISTING, nullptr);
    const auto size = size_of(handle);
    if (size < published) fail("journal_published_file_truncated");
    if (size > published) {
        FILE_END_OF_FILE_INFO end{};
        end.EndOfFile.QuadPart = static_cast<LONGLONG>(published);
        if (!::SetFileInformationByHandle(handle.get(), FileEndOfFileInfo, &end, sizeof end))
            fail_last_error("journal_unpublished_truncate_failed");
    }
    std::uint64_t at = published;
    for (const auto piece : parts) {
        write_at(handle, piece, at);
        at += piece.size();
    }
    flush(handle, "journal_file_fsync_failed");
    handle.close_checked("journal_file_close_failed");
}

// Every entry the journal creates or replaces on Windows arrives through
// publish_file or rename_directory_no_replace, whose MOVEFILE_WRITE_THROUGH
// move is documented to return only once it is on disk; nothing is left to
// flush here, and no directory-handle flush is relied on (codex J11).
// SWEGCA: user@2026-09-22:72-79
void make_entries_durable(const fs::path& directory) { (void)directory; }

// SWEGCA: user@2026-09-22:72-79
bool rename_directory_no_replace(const fs::path& from, const fs::path& to) {
    if (::MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH)) return true;
    const auto error = ::GetLastError();
    if (error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS) return false;
    ::SetLastError(error);
    fail_last_error("journal_initial_rename_failed");
}

// SWEGCA: user@2026-09-22:72-79
void remove_file(const fs::path& path) {
    if (!::DeleteFileW(path.c_str())) fail_last_error("journal_unpublished_unlink_failed");
}

// SWEGCA: user@2026-09-22:72-79
std::uint64_t allocation_unit(const fs::path& directory) {
    std::vector<wchar_t> volume(32768);
    if (!::GetVolumePathNameW(directory.c_str(), volume.data(),
                              static_cast<DWORD>(volume.size())))
        fail_last_error("journal_volume_query_failed");
    DWORD sectors_per_cluster = 0;
    DWORD bytes_per_sector = 0;
    DWORD free_clusters = 0;
    DWORD total_clusters = 0;
    if (!::GetDiskFreeSpaceW(volume.data(), &sectors_per_cluster, &bytes_per_sector,
                             &free_clusters, &total_clusters))
        fail_last_error("journal_volume_query_failed");
    const auto unit = static_cast<std::uint64_t>(sectors_per_cluster) * bytes_per_sector;
    if (unit == 0) fail("journal_allocation_unit_invalid");
    return unit;
}

// SWEGCA: user@2026-09-22:72-79
std::uint64_t process_id() noexcept { return ::GetCurrentProcessId(); }

#else

// SWEGCA: user@2026-09-22:72-79
OwnerLock::OwnerLock(const fs::path& directory) {
    const auto path = directory / "owner.lock";
    const int descriptor = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (descriptor < 0) fail_errno("journal_owner_lock_open_failed");
    if (::flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
        const int error = errno;
        ::close(descriptor);
        if (error == EWOULDBLOCK) fail("journal_already_owned");
        errno = error;
        fail_errno("journal_owner_lock_failed");
    }
    handle_ = descriptor;
}

// SWEGCA: user@2026-09-22:72-79
OwnerLock::~OwnerLock() {
    if (handle_ >= 0) ::close(static_cast<int>(handle_));
}

// SWEGCA: user@2026-09-22:72-79
void read_range(const fs::path& path, std::uint64_t published, std::uint64_t offset,
                std::span<std::byte> out, const char* missing_code) {
    if (offset > published || out.size() > published - offset) fail("journal_range_invalid");
    auto descriptor = open_file(path, O_RDONLY, missing_code);
    if (size_of(descriptor) < published) fail("journal_published_file_truncated");
    std::size_t at = 0;
    while (at < out.size()) {
        const auto read = ::pread(descriptor.get(), out.data() + at, out.size() - at,
                                  static_cast<off_t>(offset + at));
        if (read < 0) {
            if (errno == EINTR) continue;
            fail_errno("journal_file_read_failed");
        }
        if (read == 0) fail("journal_file_truncated_while_reading");
        at += static_cast<std::size_t>(read);
    }
}

// SWEGCA: user@2026-09-22:72-79
void read_exact_file(const fs::path& path, std::span<std::byte> out, const char* missing_code) {
    {
        auto descriptor = open_file(path, O_RDONLY, missing_code);
        if (size_of(descriptor) != out.size()) fail("journal_file_length_invalid");
    }
    read_range(path, out.size(), 0, out, missing_code);
}

// SWEGCA: user@2026-09-22:72-79
void publish_file_parts(const fs::path& path, std::span<const std::span<const std::byte>> parts) {
    fs::path part = path;
    part += part_suffix;
    {
        auto descriptor = open_file(part, O_WRONLY | O_CREAT | O_TRUNC, nullptr);
        std::uint64_t at = 0;
        for (const auto piece : parts) {
            write_at(descriptor, piece, at);
            at += piece.size();
        }
        flush(descriptor, "journal_file_fsync_failed");
        descriptor.close_checked("journal_file_close_failed");
    }
    if (::rename(part.c_str(), path.c_str()) != 0) fail_errno("journal_file_publish_failed");
}

// SWEGCA: user@2026-09-22:72-79
void append_parts_at_published_end(const fs::path& path, std::uint64_t published,
                                   std::span<const std::span<const std::byte>> parts) {
    auto descriptor = open_file(path, O_WRONLY, nullptr);
    const auto size = size_of(descriptor);
    if (size < published) fail("journal_published_file_truncated");
    if (size > published && ::ftruncate(descriptor.get(), static_cast<off_t>(published)) != 0)
        fail_errno("journal_unpublished_truncate_failed");
    std::uint64_t at = published;
    for (const auto piece : parts) {
        write_at(descriptor, piece, at);
        at += piece.size();
    }
    flush(descriptor, "journal_file_fsync_failed");
    descriptor.close_checked("journal_file_close_failed");
}

// SWEGCA: user@2026-09-22:72-79
void make_entries_durable(const fs::path& directory) {
    Descriptor descriptor(::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (descriptor.get() < 0) fail_errno("journal_directory_open_failed");
    flush(descriptor, "journal_directory_fsync_failed");
    descriptor.close_checked("journal_directory_close_failed");
}

// SWEGCA: user@2026-09-22:72-79
bool rename_directory_no_replace(const fs::path& from, const fs::path& to) {
    if (::renameat2(AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), RENAME_NOREPLACE) == 0)
        return true;
    if (errno == EEXIST) return false;
    fail_errno("journal_initial_rename_failed");
}

// SWEGCA: user@2026-09-22:72-79
void remove_file(const fs::path& path) {
    if (::unlink(path.c_str()) != 0) fail_errno("journal_unpublished_unlink_failed");
}

// SWEGCA: user@2026-09-22:72-79
std::uint64_t allocation_unit(const fs::path& directory) {
    struct statvfs status {};
    if (::statvfs(directory.c_str(), &status) != 0) fail_errno("journal_volume_query_failed");
    const auto unit = std::max<std::uint64_t>(status.f_frsize, status.f_bsize);
    if (unit == 0) fail("journal_allocation_unit_invalid");
    return unit;
}

// SWEGCA: user@2026-09-22:72-79
std::uint64_t process_id() noexcept { return static_cast<std::uint64_t>(::getpid()); }

#endif

// SWEGCA: user@2026-09-22:72-79
void publish_file(const fs::path& path, std::span<const std::byte> first,
                  std::span<const std::byte> second) {
    const std::span<const std::byte> parts[] = {first, second};
    publish_file_parts(path, parts);
}

// SWEGCA: user@2026-09-22:72-79
void append_at_published_end(const fs::path& path, std::uint64_t published,
                             std::span<const std::byte> bytes) {
    const std::span<const std::byte> parts[] = {bytes};
    append_parts_at_published_end(path, published, parts);
}

}  // namespace swegca::vrs::journal::io
