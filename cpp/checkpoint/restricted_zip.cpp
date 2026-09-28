#include "checkpoint/restricted_zip.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_set>

namespace swegca::checkpoint {
namespace {

constexpr std::uint32_t local_signature = 0x04034b50U;
constexpr std::uint32_t central_signature = 0x02014b50U;
constexpr std::uint32_t descriptor_signature = 0x08074b50U;
constexpr std::uint32_t zip64_eocd_signature = 0x06064b50U;
constexpr std::uint32_t zip64_locator_signature = 0x07064b50U;
constexpr std::uint32_t eocd_signature = 0x06054b50U;
constexpr std::uint16_t encrypted_flag = 1U << 0;
constexpr std::uint16_t descriptor_flag = 1U << 3;
constexpr std::uint16_t utf8_flag = 1U << 11;
constexpr std::uint16_t supported_flags = descriptor_flag | utf8_flag;
constexpr std::uint16_t stored_method = 0;
constexpr std::size_t local_header_bytes = 30;
constexpr std::size_t central_header_bytes = 46;
constexpr std::size_t descriptor_bytes = 16;
constexpr std::size_t eocd_bytes = 22;
constexpr std::size_t zip64_eocd_bytes = 56;
constexpr std::size_t zip64_locator_bytes = 20;
constexpr std::size_t maximum_eocd_search = 65535 + eocd_bytes;

[[noreturn]] void invalid(const char* message) { throw std::runtime_error(message); }

std::uint16_t u16(std::span<const std::byte> bytes, std::size_t at) {
    if (at > bytes.size() || bytes.size() - at < 2) invalid("truncated ZIP field");
    return static_cast<std::uint16_t>(std::to_integer<unsigned>(bytes[at])) |
        static_cast<std::uint16_t>(std::to_integer<unsigned>(bytes[at + 1]) << 8U);
}

std::uint32_t u32(std::span<const std::byte> bytes, std::size_t at) {
    if (at > bytes.size() || bytes.size() - at < 4) invalid("truncated ZIP field");
    std::uint32_t value = 0;
    for (unsigned index = 0; index != 4; ++index) {
        value |= std::uint32_t(std::to_integer<unsigned>(bytes[at + index])) << (8U * index);
    }
    return value;
}

std::uint64_t u64(std::span<const std::byte> bytes, std::size_t at) {
    if (at > bytes.size() || bytes.size() - at < 8) invalid("truncated ZIP field");
    std::uint64_t value = 0;
    for (unsigned index = 0; index != 8; ++index) {
        value |= std::uint64_t(std::to_integer<unsigned>(bytes[at + index])) << (8U * index);
    }
    return value;
}

std::uint64_t checked_add(std::uint64_t left, std::uint64_t right) {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) {
        throw std::overflow_error("ZIP offset overflow");
    }
    return left + right;
}

void read_exact(int fd, std::uint64_t offset, std::span<std::byte> destination) {
    std::size_t complete = 0;
    while (complete != destination.size()) {
        const auto position = checked_add(offset, complete);
        if (position > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
            throw std::overflow_error("ZIP offset does not fit off_t");
        }
        const auto count = ::pread(fd, destination.data() + complete,
            destination.size() - complete, static_cast<off_t>(position));
        if (count < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error("ZIP read failed");
        }
        if (count == 0) invalid("truncated ZIP archive");
        complete += static_cast<std::size_t>(count);
    }
}

std::vector<std::byte> read_bytes(int fd, std::uint64_t offset, std::size_t count) {
    std::vector<std::byte> result(count);
    read_exact(fd, offset, result);
    return result;
}

bool safe_name(std::string_view name) {
    if (name.empty() || name.front() == '/' || name.front() == '\\' || name.back() == '/') return false;
    if (name.find('\0') != std::string_view::npos || name.find('\\') != std::string_view::npos) return false;
    for (const unsigned char value : name) {
        // The pinned checkpoint namespace is ASCII. Reject ambiguous Unicode,
        // control bytes and platform normalization before lookup is possible.
        if (value < 0x20U || value >= 0x7fU) return false;
    }
    std::size_t start = 0;
    bool first = true;
    while (start <= name.size()) {
        const auto slash = name.find('/', start);
        const auto end = slash == std::string_view::npos ? name.size() : slash;
        const auto part = name.substr(start, end - start);
        if (part.empty() || part == "." || part == "..") return false;
        if (first && part.find(':') != std::string_view::npos) return false;
        first = false;
        if (slash == std::string_view::npos) break;
        start = slash + 1;
    }
    return true;
}

std::uint32_t crc32_update(std::uint32_t crc, std::span<const std::byte> bytes) noexcept {
    for (const auto value : bytes) {
        crc ^= std::to_integer<std::uint8_t>(value);
        for (unsigned bit = 0; bit != 8; ++bit) {
            const auto mask = std::uint32_t(0) - (crc & 1U);
            crc = (crc >> 1U) ^ (0xedb88320U & mask);
        }
    }
    return crc;
}

std::uint32_t crc32(std::span<const std::byte> bytes) noexcept {
    return ~crc32_update(0xffffffffU, bytes);
}

struct Span {
    std::uint64_t begin;
    std::uint64_t end;
};

RestrictedZipIdentity zip_identity(const struct stat& status) {
    if (!S_ISREG(status.st_mode) || status.st_size < 0) {
        invalid("ZIP archive is not a regular file");
    }
    return {
        static_cast<std::uint64_t>(status.st_dev),
        static_cast<std::uint64_t>(status.st_ino),
        static_cast<std::uint64_t>(status.st_size),
        static_cast<std::uint64_t>(status.st_mode),
        static_cast<std::int64_t>(status.st_mtim.tv_sec),
        static_cast<std::int64_t>(status.st_mtim.tv_nsec),
        static_cast<std::int64_t>(status.st_ctim.tv_sec),
        static_cast<std::int64_t>(status.st_ctim.tv_nsec),
    };
}

}  // namespace

RestrictedZip RestrictedZip::open(const std::filesystem::path& path, ZipLimits limits) {
    if (!limits.maximum_archive_bytes || !limits.maximum_member_bytes ||
        !limits.maximum_members || !limits.maximum_name_bytes) {
        throw std::invalid_argument("ZIP limits must be nonzero");
    }

    RestrictedZip result;
    result.limits_ = limits;
    result.fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (result.fd_ < 0) throw std::runtime_error("cannot open ZIP archive");
    try {
        struct stat status {};
        if (::fstat(result.fd_, &status) != 0) invalid("cannot stat ZIP archive");
        result.opened_identity_ = zip_identity(status);
        result.archive_bytes_ = result.opened_identity_.size;
        if (result.archive_bytes_ < eocd_bytes || result.archive_bytes_ > limits.maximum_archive_bytes) {
            invalid("ZIP archive size is outside limits");
        }

        const auto tail_size = static_cast<std::size_t>(
            std::min<std::uint64_t>(result.archive_bytes_, maximum_eocd_search));
        const auto tail_offset = result.archive_bytes_ - tail_size;
        const auto tail = read_bytes(result.fd_, tail_offset, tail_size);
        std::size_t eocd_at = std::numeric_limits<std::size_t>::max();
        for (std::size_t position = tail.size() - eocd_bytes + 1; position-- > 0;) {
            if (u32(tail, position) != eocd_signature) continue;
            const auto comment = u16(tail, position + 20);
            if (position + eocd_bytes + comment == tail.size()) {
                eocd_at = position;
                break;
            }
        }
        if (eocd_at == std::numeric_limits<std::size_t>::max()) invalid("valid ZIP EOCD not found");
        if (u16(tail, eocd_at + 4) != 0 || u16(tail, eocd_at + 6) != 0) {
            invalid("multi-disk ZIP is unsupported");
        }
        const auto disk_members = u16(tail, eocd_at + 8);
        const auto total_members = u16(tail, eocd_at + 10);
        const auto central_size = u32(tail, eocd_at + 12);
        const auto central_offset = u32(tail, eocd_at + 16);
        if (disk_members == 0xffffU || total_members == 0xffffU || central_size == 0xffffffffU ||
            central_offset == 0xffffffffU) invalid("ZIP64 is unsupported");
        if (disk_members != total_members || total_members == 0 || total_members > limits.maximum_members) {
            invalid("invalid ZIP member count");
        }
        const auto absolute_eocd = checked_add(tail_offset, eocd_at);
        const auto central_end = checked_add(central_offset, central_size);
        if (central_end != absolute_eocd) {
            // torch.save emits a ZIP64 EOCD and locator even while the legacy
            // EOCD values still fit ZIP32. Accept exactly that bounded trailer.
            constexpr auto trailer = zip64_eocd_bytes + zip64_locator_bytes;
            if (checked_add(central_end, trailer) != absolute_eocd) {
                invalid("ZIP central directory span mismatch");
            }
            const auto zip64 = read_bytes(result.fd_, central_end, trailer);
            if (u32(zip64, 0) != zip64_eocd_signature || u64(zip64, 4) != 44 ||
                u32(zip64, 16) != 0 || u32(zip64, 20) != 0 ||
                u64(zip64, 24) != total_members || u64(zip64, 32) != total_members ||
                u64(zip64, 40) != central_size || u64(zip64, 48) != central_offset ||
                u32(zip64, zip64_eocd_bytes) != zip64_locator_signature ||
                u32(zip64, zip64_eocd_bytes + 4) != 0 ||
                u64(zip64, zip64_eocd_bytes + 8) != central_end ||
                u32(zip64, zip64_eocd_bytes + 16) != 1) {
                invalid("ZIP64 EOCD trailer mismatch");
            }
        }

        const auto central = read_bytes(result.fd_, central_offset, central_size);
        std::size_t cursor = 0;
        std::unordered_set<std::string> names;
        std::vector<Span> local_spans;
        result.members_.reserve(total_members);
        for (std::uint32_t index = 0; index != total_members; ++index) {
            if (cursor > central.size() || central.size() - cursor < central_header_bytes ||
                u32(central, cursor) != central_signature) invalid("invalid ZIP central record");
            const auto version_made_by = u16(central, cursor + 4);
            const auto flags = u16(central, cursor + 8);
            const auto method = u16(central, cursor + 10);
            const auto expected_crc = u32(central, cursor + 16);
            const auto compressed_size = u32(central, cursor + 20);
            const auto uncompressed_size = u32(central, cursor + 24);
            const auto name_size = u16(central, cursor + 28);
            const auto extra_size = u16(central, cursor + 30);
            const auto comment_size = u16(central, cursor + 32);
            const auto disk = u16(central, cursor + 34);
            const auto external_attributes = u32(central, cursor + 38);
            const auto local_offset = u32(central, cursor + 42);
            const auto record_size = checked_add(central_header_bytes,
                checked_add(name_size, checked_add(extra_size, comment_size)));
            if (record_size > central.size() - cursor) invalid("truncated ZIP central record");
            if (disk != 0 || compressed_size == 0xffffffffU || uncompressed_size == 0xffffffffU ||
                local_offset == 0xffffffffU) invalid("ZIP64 or multi-disk member is unsupported");
            if ((flags & encrypted_flag) != 0) invalid("encrypted ZIP member is unsupported");
            if ((flags & ~supported_flags) != 0) invalid("unsupported ZIP general-purpose flags");
            if (method != stored_method) invalid("compressed ZIP member is unsupported");
            if ((version_made_by >> 8U) == 3U &&
                ((external_attributes >> 16U) & 0170000U) == 0120000U) {
                invalid("symbolic-link ZIP member is unsupported");
            }
            if (compressed_size != uncompressed_size || uncompressed_size > limits.maximum_member_bytes) {
                invalid("ZIP member size is outside stored profile");
            }
            if (name_size == 0 || name_size > limits.maximum_name_bytes) invalid("ZIP member name size");
            const auto* name_data = reinterpret_cast<const char*>(central.data() + cursor + central_header_bytes);
            std::string name(name_data, name_size);
            if (!safe_name(name)) invalid("unsafe ZIP member name");
            if (!names.insert(name).second) invalid("duplicate ZIP member name");

            if (checked_add(local_offset, local_header_bytes) > central_offset) {
                invalid("ZIP local header overlaps central directory");
            }
            const auto local = read_bytes(result.fd_, local_offset, local_header_bytes);
            if (u32(local, 0) != local_signature) invalid("invalid ZIP local record");
            const auto local_flags = u16(local, 6);
            const auto local_method = u16(local, 8);
            const auto local_crc = u32(local, 14);
            const auto local_compressed = u32(local, 18);
            const auto local_uncompressed = u32(local, 22);
            const auto local_name_size = u16(local, 26);
            const auto local_extra_size = u16(local, 28);
            if (local_flags != flags || local_method != method || local_name_size != name_size) {
                invalid("ZIP central/local metadata mismatch");
            }
            const auto local_name_offset = checked_add(local_offset, local_header_bytes);
            const auto local_name = read_bytes(result.fd_, local_name_offset, local_name_size);
            if (!std::equal(local_name.begin(), local_name.end(),
                    reinterpret_cast<const std::byte*>(name.data()))) {
                invalid("ZIP central/local name mismatch");
            }
            const auto payload_offset = checked_add(local_name_offset,
                checked_add(local_name_size, local_extra_size));
            const auto payload_end = checked_add(payload_offset, compressed_size);
            auto local_end = payload_end;
            if ((flags & descriptor_flag) != 0) {
                if (local_crc != 0 || local_compressed != 0 || local_uncompressed != 0) {
                    invalid("ZIP descriptor local fields must be zero");
                }
                local_end = checked_add(payload_end, descriptor_bytes);
                if (local_end > central_offset) invalid("ZIP descriptor overlaps central directory");
                const auto descriptor = read_bytes(result.fd_, payload_end, descriptor_bytes);
                if (u32(descriptor, 0) != descriptor_signature || u32(descriptor, 4) != expected_crc ||
                    u32(descriptor, 8) != compressed_size || u32(descriptor, 12) != uncompressed_size) {
                    invalid("ZIP data descriptor mismatch");
                }
            } else if (local_crc != expected_crc || local_compressed != compressed_size ||
                local_uncompressed != uncompressed_size) {
                invalid("ZIP central/local size or CRC mismatch");
            }
            if (payload_end > central_offset) invalid("ZIP payload overlaps central directory");
            local_spans.push_back({local_offset, local_end});
            result.members_.push_back({std::move(name), expected_crc, uncompressed_size,
                payload_offset, flags, method});
            cursor += static_cast<std::size_t>(record_size);
        }
        if (cursor != central.size()) invalid("unparsed ZIP central directory bytes");
        std::sort(local_spans.begin(), local_spans.end(), [](const Span& left, const Span& right) {
            return left.begin < right.begin;
        });
        for (std::size_t index = 1; index != local_spans.size(); ++index) {
            if (local_spans[index].begin < local_spans[index - 1].end) {
                invalid("overlapping ZIP local records");
            }
        }
        result.require_unchanged();
        return result;
    } catch (...) {
        if (result.fd_ >= 0) ::close(result.fd_);
        result.fd_ = -1;
        throw;
    }
}

RestrictedZip::RestrictedZip(RestrictedZip&& other) noexcept
    : fd_(other.fd_), archive_bytes_(other.archive_bytes_),
      opened_identity_(other.opened_identity_), limits_(other.limits_),
      members_(std::move(other.members_)) {
    other.fd_ = -1;
    other.archive_bytes_ = 0;
}

RestrictedZip& RestrictedZip::operator=(RestrictedZip&& other) noexcept {
    if (this == &other) return *this;
    if (fd_ >= 0) ::close(fd_);
    fd_ = other.fd_;
    archive_bytes_ = other.archive_bytes_;
    opened_identity_ = other.opened_identity_;
    limits_ = other.limits_;
    members_ = std::move(other.members_);
    other.fd_ = -1;
    other.archive_bytes_ = 0;
    return *this;
}

RestrictedZip::~RestrictedZip() {
    if (fd_ >= 0) ::close(fd_);
}

const ZipMember* RestrictedZip::find(std::string_view name) const noexcept {
    const auto where = std::find_if(members_.begin(), members_.end(), [name](const ZipMember& member) {
        return member.name == name;
    });
    return where == members_.end() ? nullptr : &*where;
}

RestrictedZipIdentity RestrictedZip::identity() const {
    if (fd_ < 0) invalid("ZIP archive descriptor is closed");
    struct stat status {};
    if (::fstat(fd_, &status) != 0) invalid("cannot stat ZIP archive");
    return zip_identity(status);
}

void RestrictedZip::require_unchanged() const {
    if (identity() != opened_identity_) {
        invalid("ZIP archive changed after open");
    }
}

std::vector<std::byte> RestrictedZip::read(std::string_view name) const {
    require_unchanged();
    const auto* member = find(name);
    if (member == nullptr) throw std::out_of_range("ZIP member not found");
    if (member->size > limits_.maximum_member_bytes || member->size > std::numeric_limits<std::size_t>::max()) {
        throw std::length_error("ZIP member cannot fit memory limits");
    }
    if (checked_add(member->payload_offset, member->size) > archive_bytes_) {
        invalid("ZIP member span exceeds archive");
    }
    auto bytes = read_bytes(fd_, member->payload_offset, static_cast<std::size_t>(member->size));
    if (crc32(bytes) != member->crc32) invalid("ZIP member CRC mismatch");
    require_unchanged();
    return bytes;
}

void RestrictedZip::verify(std::string_view name) const {
    require_unchanged();
    const auto* member = find(name);
    if (member == nullptr) throw std::out_of_range("ZIP member not found");
    if (checked_add(member->payload_offset, member->size) > archive_bytes_) {
        invalid("ZIP member span exceeds archive");
    }
    constexpr std::size_t chunk_bytes = 4U << 20;
    std::vector<std::byte> buffer(static_cast<std::size_t>(
        std::min<std::uint64_t>(chunk_bytes, std::max<std::uint64_t>(member->size, 1))));
    std::uint64_t complete = 0;
    std::uint32_t crc = 0xffffffffU;
    while (complete != member->size) {
        const auto count = static_cast<std::size_t>(
            std::min<std::uint64_t>(buffer.size(), member->size - complete));
        const auto view = std::span<std::byte>(buffer).first(count);
        read_exact(fd_, checked_add(member->payload_offset, complete), view);
        crc = crc32_update(crc, view);
        complete += count;
    }
    if (~crc != member->crc32) invalid("ZIP member CRC mismatch");
    require_unchanged();
}

void RestrictedZip::verify_all() const {
    for (const auto& member : members_) verify(member.name);
}

void RestrictedZip::read_archive(const std::uint64_t offset,
                                 const std::span<std::byte> destination) const {
    require_unchanged();
    if (offset > archive_bytes_ || destination.size() > archive_bytes_ - offset) {
        throw std::out_of_range("ZIP archive read outside file");
    }
    read_exact(fd_, offset, destination);
    require_unchanged();
}

}  // namespace swegca::checkpoint
