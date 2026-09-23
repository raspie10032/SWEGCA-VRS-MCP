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

// Lineage: native mechanism — one throw point for file I/O error codes.
// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3fce8b5c605166d668924baa5d4a6c49dc0:50
[[noreturn]] void fail(const char* code) { throw std::runtime_error(code); }

#if defined(_WIN32)

// Lineage: native mechanism — carries the Windows error code in a system_error.
// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3fce8b5c605166d668924baa5d4a6c49dc0:50
[[noreturn]] void fail_last_error(const char* code) {
    throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(), code);
}

// Owns one Windows handle; closing never throws.
class Handle final {
public:
    // Lineage: native mechanism — adopts one Windows handle.
    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3fce8b5c605166d668924baa5d4a6c49dc0:50
    explicit Handle(HANDLE value) noexcept : value_(value) {}
    // Lineage: native mechanism — moves ownership of the handle.
    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3fce8b5c605166d668924baa5d4a6c49dc0:50
    Handle(Handle&& other) noexcept : value_(std::exchange(other.value_, INVALID_HANDLE_VALUE)) {}
    // Lineage: weak analogy — the author closes its descriptor in finally; here the destructor, never throwing.
    // SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:45-46
    ~Handle() {
        if (value_ != INVALID_HANDLE_VALUE) ::CloseHandle(value_);
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    // Lineage: native mechanism — borrows the raw handle.
    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3fce8b5c605166d668924baa5d4a6c49dc0:50
    [[nodiscard]] HANDLE get() const noexcept { return value_; }
    // Lineage: native mechanism — tests whether a handle is held.
    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3fce8b5c605166d668924baa5d4a6c49dc0:50
    [[nodiscard]] bool valid() const noexcept { return value_ != INVALID_HANDLE_VALUE; }
    // Lineage: native mechanism — an explicit close whose failure is reported rather than lost.
    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3fce8b5c605166d668924baa5d4a6c49dc0:50
    void close_checked(const char* code) {
        const HANDLE value = std::exchange(value_, INVALID_HANDLE_VALUE);
        if (value != INVALID_HANDLE_VALUE && !::CloseHandle(value)) fail_last_error(code);
    }

private:
    HANDLE value_;
};

// Readers share delete access so HEAD can be replaced while they read.
constexpr DWORD share_all = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

// Lineage: weak analogy — the author treats a segment missing on open as absent; here it raises the caller's code.
// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:318-322
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

// Lineage: weak analogy — the author reads st_size of an opened segment; here GetFileSizeEx on the handle.
// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:324
std::uint64_t size_of(const Handle& handle) {
    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(handle.get(), &size)) fail_last_error("journal_file_stat_failed");
    return static_cast<std::uint64_t>(size.QuadPart);
}

// Lineage: native mechanism — splits a 64-bit offset into OVERLAPPED halves for positional I/O.
// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3fce8b5c605166d668924baa5d4a6c49dc0:50
OVERLAPPED at_offset(std::uint64_t offset) noexcept {
    OVERLAPPED overlapped{};
    overlapped.Offset = static_cast<DWORD>(offset & 0xffffffffu);
    overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32);
    return overlapped;
}

// Positional write in DWORD-sized chunks; a zero-byte write fails.
// Lineage: weak analogy — the author loops os.write until all is written, failing on zero; here positional writes.
// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:62-69
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

// Lineage: weak analogy — the author fsyncs each written file inline; here one checked flush.
// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3fce8b5c605166d668924baa5d4a6c49dc0:576
// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:58
void flush(const Handle& handle, const char* code) {
    if (!::FlushFileBuffers(handle.get())) fail_last_error(code);
}

#else

// Lineage: native mechanism — carries errno in a system_error.
// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3fce8b5c605166d668924baa5d4a6c49dc0:50
[[noreturn]] void fail_errno(const char* code) {
    throw std::system_error(errno, std::generic_category(), code);
}

// Owns one POSIX descriptor; closing never throws.
class Descriptor final {
public:
    // Lineage: native mechanism — adopts one POSIX descriptor.
    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3fce8b5c605166d668924baa5d4a6c49dc0:50
    explicit Descriptor(int value) noexcept : value_(value) {}
    // Lineage: native mechanism — moves ownership of the descriptor.
    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3fce8b5c605166d668924baa5d4a6c49dc0:50
    Descriptor(Descriptor&& other) noexcept : value_(std::exchange(other.value_, -1)) {}
    // Lineage: weak analogy — the author closes its descriptor in finally; here the destructor, never throwing.
    // SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:45-46
    ~Descriptor() {
        if (value_ >= 0) ::close(value_);
    }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    // Lineage: native mechanism — borrows the raw descriptor.
    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3fce8b5c605166d668924baa5d4a6c49dc0:50
    [[nodiscard]] int get() const noexcept { return value_; }
    // Lineage: native mechanism — an explicit close whose failure is reported rather than lost.
    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3fce8b5c605166d668924baa5d4a6c49dc0:50
    void close_checked(const char* code) {
        const int value = std::exchange(value_, -1);
        if (value >= 0 && ::close(value) != 0) fail_errno(code);
    }

private:
    int value_;
};

// Lineage: weak analogy — the author treats a segment missing on open as absent; here it raises the caller's code.
// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:318-322
Descriptor open_file(const fs::path& path, int flags, const char* missing_code) {
    Descriptor descriptor(::open(path.c_str(), flags | O_CLOEXEC, 0600));
    if (descriptor.get() < 0) {
        if (missing_code != nullptr && errno == ENOENT) fail(missing_code);
        fail_errno("journal_file_open_failed");
    }
    return descriptor;
}

// Lineage: weak analogy — the author reads st_size of an opened segment; here a non-regular file also fails.
// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:324
std::uint64_t size_of(const Descriptor& descriptor) {
    struct stat status {};
    if (::fstat(descriptor.get(), &status) != 0) fail_errno("journal_file_stat_failed");
    if (!S_ISREG(status.st_mode)) fail("journal_file_not_regular");
    return static_cast<std::uint64_t>(status.st_size);
}

// Positional write; a zero-byte write fails rather than looping (codex J6).
// Lineage: weak analogy — the author loops os.write until all is written, failing on zero; here positional writes.
// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:62-69
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

// Lineage: weak analogy — the author fsyncs each written file inline; here one checked flush.
// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3fce8b5c605166d668924baa5d4a6c49dc0:576
// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:58
void flush(const Descriptor& descriptor, const char* code) {
    if (::fsync(descriptor.get()) != 0) fail_errno(code);
}

#endif

}  // namespace

#if defined(_WIN32)

// Lineage: weak analogy — the author polls a non-blocking exclusive lock; here one try, and a live owner fails.
// SWEGCA: src/swegca_vrs2/native_lock.py@c06092a:64-77
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

// Lineage: weak analogy — the author unlocks then closes the lock file; here closing it releases the lock.
// SWEGCA: src/swegca_vrs2/native_lock.py@c06092a:88-92
OwnerLock::~OwnerLock() {
    if (handle_ != -1) ::CloseHandle(reinterpret_cast<HANDLE>(handle_));
}

// Lineage: weak analogy — the author preads a capsule at its offset, failing short reads; here a published range.
// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3fce8b5c605166d668924baa5d4a6c49dc0:578-580
// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:663-665
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

// Lineage: weak analogy — the author reads a whole checkpoint, checking a minimum length; here the exact length.
// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:263-265
void read_exact_file(const fs::path& path, std::span<std::byte> out, const char* missing_code) {
    {
        auto handle = open_file(path, GENERIC_READ, OPEN_EXISTING, missing_code);
        if (size_of(handle) != out.size()) fail("journal_file_length_invalid");
    }
    read_range(path, out.size(), 0, out, missing_code);
}

// Lineage: weak analogy — the author writes a random O_EXCL temp and deletes it on failure; here a fixed .part name, kept.
// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:53-59
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

// Lineage: weak analogy — the author appends, fsyncs and cuts back on failure; here unpublished bytes are cut first.
// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3fce8b5c605166d668924baa5d4a6c49dc0:579-581
// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:233-242
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
// Lineage: direct — the author's directory fsync returns at once on Windows; so does this.
// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:39-41
void make_entries_durable(const fs::path& directory) { (void)directory; }

// Lineage: weak analogy — the author refuses an existing generation via mkdir; here a built one moves to a free name.
// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3fce8b5c605166d668924baa5d4a6c49dc0:587-588
// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:103-104
bool rename_directory_no_replace(const fs::path& from, const fs::path& to) {
    if (::MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH)) return true;
    const auto error = ::GetLastError();
    if (error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS) return false;
    ::SetLastError(error);
    fail_last_error("journal_initial_rename_failed");
}

// Lineage: weak analogy — the author unlinks a leftover temporary; here an unpublished file, which must exist.
// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3fce8b5c605166d668924baa5d4a6c49dc0:580-581
// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:61-62
void remove_file(const fs::path& path) {
    if (!::DeleteFileW(path.c_str())) fail_last_error("journal_unpublished_unlink_failed");
}

// Lineage: native mechanism — file-system allocation unit, so the host's budget sees disk use.
// SWEGCA: docs/SWEGCA_CPP_VRS_LAYER_PLAN.md@472d23225c973fa0a33581afd6bd9026df6fc98a:170-173
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

// Lineage: native mechanism — process id that keeps a sibling build directory's name unique.
// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3fce8b5c605166d668924baa5d4a6c49dc0:50
std::uint64_t process_id() noexcept { return ::GetCurrentProcessId(); }

#else

// Lineage: weak analogy — the author polls a non-blocking exclusive lock; here one try, and a live owner fails.
// SWEGCA: src/swegca_vrs2/native_lock.py@c06092a:64-77
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

// Lineage: weak analogy — the author unlocks then closes the lock file; here closing it releases the lock.
// SWEGCA: src/swegca_vrs2/native_lock.py@c06092a:88-92
OwnerLock::~OwnerLock() {
    if (handle_ >= 0) ::close(static_cast<int>(handle_));
}

// Lineage: weak analogy — the author preads a capsule at its offset, failing short reads; here a published range.
// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3fce8b5c605166d668924baa5d4a6c49dc0:578-580
// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:663-665
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

// Lineage: weak analogy — the author reads a whole checkpoint, checking a minimum length; here the exact length.
// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:263-265
void read_exact_file(const fs::path& path, std::span<std::byte> out, const char* missing_code) {
    {
        auto descriptor = open_file(path, O_RDONLY, missing_code);
        if (size_of(descriptor) != out.size()) fail("journal_file_length_invalid");
    }
    read_range(path, out.size(), 0, out, missing_code);
}

// Lineage: weak analogy — the author writes a random O_EXCL temp and deletes it on failure; here a fixed .part name, kept.
// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:53-59
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

// Lineage: weak analogy — the author appends, fsyncs and cuts back on failure; here unpublished bytes are cut first.
// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3fce8b5c605166d668924baa5d4a6c49dc0:579-581
// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:233-242
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

// Lineage: direct — open the directory read-only, fsync it and close it.
// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:39-46
void make_entries_durable(const fs::path& directory) {
    Descriptor descriptor(::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (descriptor.get() < 0) fail_errno("journal_directory_open_failed");
    flush(descriptor, "journal_directory_fsync_failed");
    descriptor.close_checked("journal_directory_close_failed");
}

// Lineage: weak analogy — the author refuses an existing generation via mkdir; here a built one moves to a free name.
// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3fce8b5c605166d668924baa5d4a6c49dc0:587-588
// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:103-104
bool rename_directory_no_replace(const fs::path& from, const fs::path& to) {
    if (::renameat2(AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), RENAME_NOREPLACE) == 0)
        return true;
    if (errno == EEXIST) return false;
    fail_errno("journal_initial_rename_failed");
}

// Lineage: weak analogy — the author unlinks a leftover temporary; here an unpublished file, which must exist.
// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3fce8b5c605166d668924baa5d4a6c49dc0:580-581
// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:61-62
void remove_file(const fs::path& path) {
    if (::unlink(path.c_str()) != 0) fail_errno("journal_unpublished_unlink_failed");
}

// Lineage: native mechanism — file-system allocation unit, so the host's budget sees disk use.
// SWEGCA: docs/SWEGCA_CPP_VRS_LAYER_PLAN.md@472d23225c973fa0a33581afd6bd9026df6fc98a:170-173
std::uint64_t allocation_unit(const fs::path& directory) {
    struct statvfs status {};
    if (::statvfs(directory.c_str(), &status) != 0) fail_errno("journal_volume_query_failed");
    const auto unit = std::max<std::uint64_t>(status.f_frsize, status.f_bsize);
    if (unit == 0) fail("journal_allocation_unit_invalid");
    return unit;
}

// Lineage: native mechanism — process id that keeps a sibling build directory's name unique.
// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3fce8b5c605166d668924baa5d4a6c49dc0:50
std::uint64_t process_id() noexcept { return static_cast<std::uint64_t>(::getpid()); }

#endif

// Lineage: weak analogy — the author publishes one body; here two spans go through publish_file_parts.
// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:49-62
void publish_file(const fs::path& path, std::span<const std::byte> first,
                  std::span<const std::byte> second) {
    const std::span<const std::byte> parts[] = {first, second};
    publish_file_parts(path, parts);
}

// Lineage: weak analogy — the author appends one frame; here one span goes through the parts form.
// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3fce8b5c605166d668924baa5d4a6c49dc0:579-581
// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:233-242
void append_at_published_end(const fs::path& path, std::uint64_t published,
                             std::span<const std::byte> bytes) {
    const std::span<const std::byte> parts[] = {bytes};
    append_parts_at_published_end(path, published, parts);
}

}  // namespace swegca::vrs::journal::io
