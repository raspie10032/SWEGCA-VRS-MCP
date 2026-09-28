#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace swegca::checkpoint {

// Deliberately narrow ZIP32 profile for the pinned PyTorch checkpoints.
// It supports stored members only and never invokes a decompressor.
struct ZipLimits {
    std::uint64_t maximum_archive_bytes = 2ULL << 30;
    std::uint64_t maximum_member_bytes = 1ULL << 30;
    std::uint32_t maximum_members = 4096;
    std::uint32_t maximum_name_bytes = 4096;
};

struct ZipMember {
    std::string name;
    std::uint32_t crc32 = 0;
    std::uint64_t size = 0;
    std::uint64_t payload_offset = 0;
    std::uint16_t flags = 0;
    std::uint16_t method = 0;
};

// Identity of the already-open regular file. The descriptor pins the inode;
// these fields additionally detect in-place writes while an audited archive
// is being parsed or consumed.
struct RestrictedZipIdentity final {
    std::uint64_t device = 0;
    std::uint64_t inode = 0;
    std::uint64_t size = 0;
    std::uint64_t mode = 0;
    std::int64_t modification_seconds = 0;
    std::int64_t modification_nanoseconds = 0;
    std::int64_t change_seconds = 0;
    std::int64_t change_nanoseconds = 0;

    friend bool operator==(const RestrictedZipIdentity&,
                           const RestrictedZipIdentity&) = default;
};

class RestrictedZip final {
public:
    [[nodiscard]] static RestrictedZip open(
        const std::filesystem::path& path, ZipLimits limits = {});

    RestrictedZip(const RestrictedZip&) = delete;
    RestrictedZip& operator=(const RestrictedZip&) = delete;
    RestrictedZip(RestrictedZip&& other) noexcept;
    RestrictedZip& operator=(RestrictedZip&& other) noexcept;
    ~RestrictedZip();

    [[nodiscard]] std::span<const ZipMember> members() const noexcept { return members_; }
    [[nodiscard]] const ZipMember* find(std::string_view name) const noexcept;

    // Reads exactly one validated stored member and verifies its CRC32 before
    // returning any bytes. Missing members and integrity failures throw.
    [[nodiscard]] std::vector<std::byte> read(std::string_view name) const;

    // Streams a validated member through a bounded buffer and checks its CRC.
    // This is used by the checkpoint gate so large tensor storage records do
    // not have to be materialized merely to prove archive integrity.
    void verify(std::string_view name) const;
    void verify_all() const;

    // Bounded access to the exact descriptor validated by open().
    void read_archive(std::uint64_t offset, std::span<std::byte> destination) const;

    [[nodiscard]] std::uint64_t archive_bytes() const noexcept { return archive_bytes_; }
    [[nodiscard]] RestrictedZipIdentity identity() const;

private:
    void require_unchanged() const;
    RestrictedZip() = default;
    int fd_ = -1;
    std::uint64_t archive_bytes_ = 0;
    RestrictedZipIdentity opened_identity_{};
    ZipLimits limits_{};
    std::vector<ZipMember> members_;
};

}  // namespace swegca::checkpoint
