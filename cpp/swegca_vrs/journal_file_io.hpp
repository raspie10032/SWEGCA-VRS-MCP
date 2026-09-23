#pragma once

#include "swegca_vrs/allocation.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>

// Durable file primitives of the native journal, one implementation per
// platform (POSIX and Windows) behind the same contract, so the journal's
// format and publication order are identical everywhere the nano-core runs
// (user@2026-09-23: any environment; no "unsupported platform" omission).
// Every failure throws; nothing here decides what is published.
//
// Lower-journal entry publication protocol (codex J11): a new name appears by
// an atomic move of a fully written and flushed `.part` file.
//   POSIX:   write, fsync(file), rename, then `make_entries_durable` fsyncs
//            the directory.
//   Windows: write, FlushFileBuffers(file), MoveFileExW with
//            MOVEFILE_WRITE_THROUGH, which is documented not to return until
//            the move is on disk; `make_entries_durable` has nothing left to
//            do, and no undocumented directory flush is relied on.
// Main's pending commit marker is different: Main first creates a unique
// attempt directory exclusively, then creates and flushes the pending file
// there and moves it after Main's owner swap. The author's final replace is
// safe because that directory was new; this native move also refuses an
// existing destination.
namespace swegca::vrs::journal::io {

inline constexpr const char* part_suffix = ".part";

// Selects the OwnerLock constructor that opens only an existing lock file.
struct ExistingLock {};
inline constexpr ExistingLock existing_lock{};

// Exclusive, process-held lock on `<dir>/owner.lock`, taken without waiting.
// A second live owner fails with `journal_already_owned`.
class OwnerLock final {
public:
    explicit OwnerLock(const std::filesystem::path& directory);
    // Opens only an existing regular `owner.lock`: nothing is created, and
    // when `owner.lock` itself is a symbolic link (a reparse point on
    // Windows) it is refused, not followed, in the same call that opens it,
    // so that name cannot be swapped between a check and the open. Links
    // in `directory` and above are followed, as every journal path is. A
    // missing lock, a link, or an opened entry that is not a regular file
    // fails with `journal_root_lock_missing`; one that cannot be opened as
    // a file at all (e.g. a directory) fails with
    // `journal_owner_lock_open_failed`.
    OwnerLock(const std::filesystem::path& directory, ExistingLock);
    ~OwnerLock();
    OwnerLock(const OwnerLock&) = delete;
    OwnerLock& operator=(const OwnerLock&) = delete;

private:
    std::intptr_t handle_ = -1;
};

// Fills `out` with [offset, offset + out.size()) of a file whose published
// length is `published`; the file may be longer (unpublished bytes), never
// shorter. The caller owns (and has charged) `out`; nothing is allocated here.
void read_range(const std::filesystem::path& path, std::uint64_t published, std::uint64_t offset,
                std::span<std::byte> out, const char* missing_code);

// Fills `out` with a whole file that must be exactly `out.size()` bytes long.
void read_exact_file(const std::filesystem::path& path, std::span<std::byte> out,
                     const char* missing_code);

// Writes `first` then `second` to `<path>.part`, flushes it, then atomically moves it to
// `path`, replacing an unpublished file of that name. When this returns on
// Windows the entry is durable; on POSIX it is durable once
// `make_entries_durable` has run on the directory. A failure before the move
// leaves `path` untouched.
void publish_file(const std::filesystem::path& path, std::span<const std::byte> first,
                  std::span<const std::byte> second = {});
// The same with any number of parts written back to back, flushed once.
void publish_file_parts(const std::filesystem::path& path,
                        std::span<const std::span<const std::byte>> parts);

// Main's append-only commit marker has a different publication order from a
// replaceable journal HEAD. Main must first create a unique attempt directory
// exclusively. Create the pending name there exclusively and fsync its bytes
// before Main swaps the live owner pair. A failure after creation leaves that
// pending file for Main to reconcile.
// SWEGCA: src/tinylm_slicer/mosaic_paper_resident_assimilation.py@3bddcb7:491-535
void write_new_file_fsynced(const std::filesystem::path& path,
                            std::span<const std::byte> bytes);

// After Main's owner swap, move a pending marker to its committed name only
// when the destination is absent. This no-replace guard is stricter than the
// author's replace inside a new attempt directory. False means the destination
// exists and leaves `from` untouched. A false result or exception after the
// owner swap must leave the new pair live and block writes until reconciliation.
// On POSIX the caller must then fsync the attempt directory; the Windows move
// uses WRITE_THROUGH.
// SWEGCA: src/tinylm_slicer/mosaic_paper_resident_assimilation.py@3bddcb7:491-535
[[nodiscard]] bool rename_file_no_replace(const std::filesystem::path& from,
                                          const std::filesystem::path& to);

// Cuts unpublished bytes past `published`, appends `bytes` there and makes
// them durable. A file shorter than `published` fails closed. With no bytes
// it only cuts.
void append_at_published_end(const std::filesystem::path& path, std::uint64_t published,
                             std::span<const std::byte> bytes);
// The same with any number of parts written back to back, flushed once.
void append_parts_at_published_end(const std::filesystem::path& path, std::uint64_t published,
                                   std::span<const std::span<const std::byte>> parts);

// Makes the directory's entries (new names, moves) durable; see the protocol
// above for what each platform needs here.
void make_entries_durable(const std::filesystem::path& directory);

// Creates Main's unique attempt directory only when the name is free. False
// leaves an existing path untouched (including a non-directory). POSIX uses
// mode 0700; Windows creates with the parent's ACL. No marker is selected by
// this operation. Main keeps the directory outside the lower journal tree.
// SWEGCA: src/tinylm_slicer/mosaic_paper_resident_assimilation.py@3bddcb7:476
[[nodiscard]] bool create_directory_new(const std::filesystem::path& path);

// Renames a directory only if `to` does not exist. Returns false (leaving
// both untouched) when `to` exists. Durable per the protocol above.
[[nodiscard]] bool rename_directory_no_replace(const std::filesystem::path& from,
                                               const std::filesystem::path& to);

// Removes a file that exists.
void remove_file(const std::filesystem::path& path);

// Allocation granularity of the file system holding `directory` in bytes
// (the cluster or fundamental block size), used to charge on-disk use.
// Windows' volume-path query buffer uses the VRS host's memory allocator.
[[nodiscard]] std::uint64_t allocation_unit(const std::filesystem::path& directory,
                                            const AllocationContext& memory);

[[nodiscard]] std::uint64_t process_id() noexcept;

}  // namespace swegca::vrs::journal::io
