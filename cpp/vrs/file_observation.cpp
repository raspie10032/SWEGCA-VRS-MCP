#include "vrs/file_observation.hpp"

#include <algorithm>
#include <cerrno>
#include <vector>
#include <unistd.h>

namespace swegca::vrs {
namespace {
constexpr std::size_t chunk_bytes = 32768;

bool same_file_version(const struct stat& a, const struct stat& b) noexcept {
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_mode == b.st_mode &&
        a.st_size == b.st_size && a.st_mtim.tv_sec == b.st_mtim.tv_sec &&
        a.st_mtim.tv_nsec == b.st_mtim.tv_nsec && a.st_ctim.tv_sec == b.st_ctim.tv_sec &&
        a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
}

bool read_chunk(int fd, std::span<std::byte> output, FileReadObservation& observation,
    architecture::Sha256& digest, TransferBudget& transfer, int& error) {
    std::size_t filled = 0;
    while (filled < output.size()) {
        const auto requested = output.size() - filled;
        transfer.wait(requested);
        const auto n = ::pread(fd, output.data() + filled, requested,
            static_cast<off_t>(observation.bytes_read));
        if (n < 0) {
            if (errno == EINTR) continue;
            error = errno; return false;
        }
        if (n == 0) { error = EIO; return false; }
        digest.update(output.subspan(filled, static_cast<std::size_t>(n)));
        filled += static_cast<std::size_t>(n);
        observation.bytes_read += static_cast<std::uint64_t>(n);
    }
    return true;
}

bool confirm_end(int fd, std::uint64_t offset, TransferBudget& transfer, int& error) {
    std::byte byte{};
    for (;;) {
        transfer.wait(1);
        const auto n = ::pread(fd, &byte, 1, static_cast<off_t>(offset));
        if (n < 0 && errno == EINTR) continue;
        if (n == 0) return true;
        error = n < 0 ? errno : EIO; return false;
    }
}
}

FileEqualityObservation observe_file_equality(int left, int right, std::uint64_t max_bytes,
    std::pmr::memory_resource& memory, TransferBudget& transfer) {
    FileEqualityObservation result;
    if (::fstat(left, &result.left.before) || ::fstat(right, &result.right.before)) {
        result.io_error = errno; return result;
    }
    for (const auto* file : {&result.left, &result.right}) {
        if (!S_ISREG(file->before.st_mode) || file->before.st_size < 0) {
            result.io_error = EINVAL; return result;
        }
        if (static_cast<std::uint64_t>(file->before.st_size) > max_bytes) {
            result.io_error = EFBIG; return result;
        }
    }
    std::pmr::vector<std::byte> scratch(2 * chunk_bytes, &memory);
    auto a = std::span(scratch).first(chunk_bytes);
    auto b = std::span(scratch).last(chunk_bytes);
    architecture::Sha256 left_digest, right_digest;
    bool equal = result.left.before.st_size == result.right.before.st_size;
    while (result.left.bytes_read < static_cast<std::uint64_t>(result.left.before.st_size) ||
           result.right.bytes_read < static_cast<std::uint64_t>(result.right.before.st_size)) {
        const auto left_count = std::min<std::uint64_t>(chunk_bytes,
            static_cast<std::uint64_t>(result.left.before.st_size) - result.left.bytes_read);
        const auto right_count = std::min<std::uint64_t>(chunk_bytes,
            static_cast<std::uint64_t>(result.right.before.st_size) - result.right.bytes_read);
        if (!read_chunk(left, a.first(left_count), result.left, left_digest, transfer, result.io_error) ||
            !read_chunk(right, b.first(right_count), result.right, right_digest, transfer, result.io_error))
            return result;
        // Compare bytes, not digests. Continue through both originals after a
        // mismatch so a later failed/changed read cannot become refutation.
        if (left_count != right_count || !std::equal(a.begin(), a.begin() + left_count, b.begin())) equal = false;
    }
    result.left.digest = left_digest.finish(); result.right.digest = right_digest.finish();
    // st_size alone is not proof of EOF (for example procfs regular files).
    if (!confirm_end(left, result.left.bytes_read, transfer, result.io_error) ||
        !confirm_end(right, result.right.bytes_read, transfer, result.io_error)) return result;
    if (::fstat(left, &result.left.after) || ::fstat(right, &result.right.after)) {
        result.io_error = errno; return result;
    }
    result.complete = true;
    result.stable = same_file_version(result.left.before, result.left.after) &&
        same_file_version(result.right.before, result.right.after);
    result.outcome = architecture::kernel::observe_content_equality(result.complete, result.stable, equal);
    return result;
}
} // namespace swegca::vrs
