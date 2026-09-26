#include "vrs/experience_block.hpp"

#include "swegca_architecture/sha256.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace swegca::vrs {
namespace {
using architecture::DigestBytes;
using architecture::Sha256;
constexpr std::string_view block_magic = "SWGCBLK1";
constexpr std::string_view record_magic = "SWGCEXP1";
constexpr std::string_view end_magic = "SWGCEND1";
constexpr std::size_t prefix_bytes = 64;
constexpr std::size_t trailer_bytes = 48;

[[noreturn]] void io_error(const char* operation) {
    throw std::system_error(errno, std::generic_category(), operation);
}

void put_u64(std::span<std::byte> bytes, std::size_t offset, std::uint64_t value) noexcept {
    for (unsigned i = 0; i < 8; ++i) bytes[offset + i] = std::byte(value >> (8 * i));
}

std::uint64_t get_u64(std::span<const std::byte> bytes, std::size_t offset) noexcept {
    std::uint64_t result = 0;
    for (unsigned i = 0; i < 8; ++i)
        result |= std::uint64_t(std::to_integer<unsigned char>(bytes[offset + i])) << (8 * i);
    return result;
}

std::span<const std::byte> as_bytes(std::string_view value) noexcept {
    return {reinterpret_cast<const std::byte*>(value.data()), value.size()};
}

bool magic_is(std::span<const std::byte> bytes, std::string_view expected) noexcept {
    return bytes.size() >= expected.size() &&
        std::memcmp(bytes.data(), expected.data(), expected.size()) == 0;
}

std::uint64_t file_size(int fd) {
    struct stat information {};
    if (::fstat(fd, &information) < 0) io_error("experience fstat");
    if (!S_ISREG(information.st_mode) || information.st_size < 0)
        throw std::runtime_error("experience block is not a regular file");
    return static_cast<std::uint64_t>(information.st_size);
}

void read_exact(int fd, std::span<std::byte> output, std::uint64_t offset, StorageBudget* storage) {
    while (!output.empty()) {
        if(storage)storage->transfer().wait(std::min<std::size_t>(output.size(), TransferBudget::chunk_bytes));
        const auto count = ::pread(fd, output.data(), std::min<std::size_t>(output.size(), 1U << 20),
                                   static_cast<off_t>(offset));
        if (count < 0) {
            if (errno == EINTR) continue;
            io_error("experience pread");
        }
        if (count == 0) throw std::runtime_error("incomplete experience record");
        output = output.subspan(static_cast<std::size_t>(count));
        offset += static_cast<std::uint64_t>(count);
    }
}

void write_exact(int fd, std::span<const std::byte> input, std::uint64_t offset, StorageBudget* storage) {
    while (!input.empty()) {
        if(storage)storage->transfer().wait(std::min<std::size_t>(input.size(), TransferBudget::chunk_bytes));
        const auto count = ::pwrite(fd, input.data(), std::min<std::size_t>(input.size(), 1U << 20),
                                    static_cast<off_t>(offset));
        if (count < 0) {
            if (errno == EINTR) continue;
            io_error("experience pwrite");
        }
        if (count == 0) throw std::runtime_error("zero-length experience write");
        input = input.subspan(static_cast<std::size_t>(count));
        offset += static_cast<std::uint64_t>(count);
    }
}

void sync_data(int fd) {
    while (::fdatasync(fd) < 0) {
        if (errno != EINTR) io_error("experience fdatasync");
    }
}

// Validate all lengths before allocating or converting them to pointer offsets.
std::uint64_t record_length(std::span<const std::byte> prefix, std::uint64_t remaining_capacity) {
    if (!magic_is(prefix, record_magic)) throw std::runtime_error("invalid experience record magic");
    const auto total = get_u64(prefix, 8);
    if (total < ExperienceBlock::record_overhead || total > remaining_capacity)
        throw std::runtime_error("invalid experience record extent");
    std::uint64_t remaining = total - ExperienceBlock::record_overhead;
    for (const auto field : {32U, 40U, 48U, 56U}) {
        const auto count = get_u64(prefix, field);
        if (count > remaining || (field != 56 && count == 0))
            throw std::runtime_error("invalid experience field extent");
        remaining -= count;
    }
    if (remaining != 0) throw std::runtime_error("experience lengths disagree");
    return total;
}

void check_trailer(std::span<const std::byte> trailer, std::uint64_t size, const DigestBytes& digest) {
    if (!std::equal(digest.begin(), digest.end(), trailer.begin()) ||
        !magic_is(trailer.subspan(32), end_magic) || get_u64(trailer, 40) != size)
        throw std::runtime_error("experience digest or completion marker mismatch");
}

std::string_view text_at(std::span<const std::byte> encoded, std::size_t offset, std::size_t count) noexcept {
    return {reinterpret_cast<const char*>(encoded.data() + offset), count};
}
}  // namespace

DigestBytes extend_experience_digest(const DigestBytes& previous, const ExperienceLocation& location) noexcept {
    // A fresh hash, fixed-size inputs, one finish: no fallible hash operation.
    Sha256 hash;
    hash.update("SWEGCA experience address chain v1");
    hash.update(previous); hash.update(location.block); hash.update(location.digest);
    std::array<std::byte, 16> extent{};
    put_u64(extent, 0, location.offset); put_u64(extent, 8, location.bytes);
    hash.update(extent);
    return hash.finish();
}

OriginalExperienceView StoredExperience::view() const noexcept {
    if (encoded_.size() < ExperienceBlock::record_overhead) return {};
    const auto session_bytes = static_cast<std::size_t>(get_u64(encoded_, 32));
    const auto source_bytes = static_cast<std::size_t>(get_u64(encoded_, 40));
    const auto media_bytes = static_cast<std::size_t>(get_u64(encoded_, 48));
    const auto content_bytes = static_cast<std::size_t>(get_u64(encoded_, 56));
    auto at = prefix_bytes;
    const auto session = text_at(encoded_, at, session_bytes); at += session_bytes;
    const auto source = text_at(encoded_, at, source_bytes); at += source_bytes;
    const auto media = text_at(encoded_, at, media_bytes); at += media_bytes;
    return {get_u64(encoded_, 16), get_u64(encoded_, 24), session, source, media,
            std::span<const std::byte>(encoded_).subspan(at, content_bytes)};
}

ExperienceBlock::ExperienceBlock(ExperienceBlock&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)), identity_(other.identity_), capacity_(other.capacity_),
      end_(other.end_), writable_(std::exchange(other.writable_, false)), storage_(std::exchange(other.storage_, nullptr)) {}

ExperienceBlock& ExperienceBlock::operator=(ExperienceBlock&& other) noexcept {
    if (this != &other) {
        if (fd_ >= 0) ::close(fd_);
        fd_ = std::exchange(other.fd_, -1);
        identity_ = other.identity_;
        capacity_ = other.capacity_;
        end_ = other.end_;
        writable_ = std::exchange(other.writable_, false);
        storage_ = std::exchange(other.storage_, nullptr);
    }
    return *this;
}

ExperienceBlock::~ExperienceBlock() { if (fd_ >= 0) ::close(fd_); }

ExperienceBlock ExperienceBlock::create(const std::filesystem::path& path,
    const DigestBytes& identity, std::uint64_t capacity, StorageBudget* storage) {
    if (identity == architecture::zero_digest_bytes || capacity < header_bytes + record_overhead ||
        capacity > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()))
        throw std::invalid_argument("invalid experience block identity or capacity");
    StorageBudget::Reservation charge(storage, header_bytes);
    ExperienceBlock block; block.storage_ = storage;
    block.fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (block.fd_ < 0) io_error("create experience block");
    if (::flock(block.fd_, LOCK_EX | LOCK_NB) < 0) io_error("lock new experience block");
    block.identity_ = identity;
    block.capacity_ = capacity;
    std::array<std::byte, header_bytes> header{};
    std::memcpy(header.data(), block_magic.data(), block_magic.size());
    std::copy(identity.begin(), identity.end(), header.begin() + 8);
    put_u64(header, 40, capacity);
    const auto digest = Sha256::of(std::span<const std::byte>(header).first(48));
    std::copy(digest.begin(), digest.end(), header.begin() + 48);
    charge.retain();
    write_exact(block.fd_, header, 0, block.storage_);
    sync_data(block.fd_);
    const auto parent = path.has_parent_path() ? path.parent_path() : std::filesystem::path(".");
    const auto directory = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0) io_error("open experience block directory");
    int result;
    do { result = ::fsync(directory); } while (result < 0 && errno == EINTR);
    const auto saved_errno = errno;
    ::close(directory);
    if (result < 0) { errno = saved_errno; io_error("sync experience block directory"); }
    block.writable_ = true;
    return block;
}

ExperienceBlock ExperienceBlock::open(const std::filesystem::path& path, bool writer, StorageBudget* storage) {
    ExperienceBlock block; block.storage_=storage;
    block.fd_ = ::open(path.c_str(), (writer ? O_RDWR : O_RDONLY) | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (block.fd_ < 0) io_error("open experience block");
    const auto size = file_size(block.fd_);
    if (writer && ::flock(block.fd_, LOCK_EX | LOCK_NB) < 0) io_error("lock experience block");
    std::array<std::byte, header_bytes> header;
    read_exact(block.fd_, header, 0, block.storage_);
    const auto digest = Sha256::of(std::span<const std::byte>(header).first(48));
    if (!magic_is(header, block_magic) || !std::equal(digest.begin(), digest.end(), header.begin() + 48))
        throw std::runtime_error("invalid experience block header");
    std::copy_n(header.begin() + 8, block.identity_.size(), block.identity_.begin());
    block.capacity_ = get_u64(header, 40);
    if (block.identity_ == architecture::zero_digest_bytes ||
        block.capacity_ < header_bytes + record_overhead || size > block.capacity_ ||
        block.capacity_ > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()))
        throw std::runtime_error("invalid experience block bounds");
    if (writer) {
        const auto recovery = block.inspect();
        block.end_ = recovery.complete_bytes;
        block.writable_ = recovery.unfinished_bytes == 0;
    }
    return block;
}

ExperienceBlock ExperienceBlock::open_reader(const std::filesystem::path& path, StorageBudget* storage) { return open(path, false, storage); }
ExperienceBlock ExperienceBlock::open_writer(const std::filesystem::path& path, StorageBudget* storage) {
    auto block = open(path, true, storage); block.storage_ = storage; return block;
}

ExperienceLocation ExperienceBlock::append(const OriginalExperienceView& experience) {
    const std::array parts{experience.content};
    return append_parts(experience, parts);
}
ExperienceLocation ExperienceBlock::append_parts(const OriginalExperienceView& experience,
    std::span<const std::span<const std::byte>> parts) {
    if (!writable_ || fd_ < 0) throw std::logic_error("experience block cannot append");
    if (experience.session.empty() || experience.source.empty() || experience.media_type.empty())
        throw std::invalid_argument("experience provenance is incomplete");
    if (capacity_ - end_ < record_overhead) throw std::length_error("experience block is full");
    auto remaining = capacity_ - end_ - record_overhead;
    for (const auto count : {experience.session.size(), experience.source.size(),
                             experience.media_type.size()}) {
        if (count > remaining) throw std::length_error("experience does not fit in block");
        remaining -= count;
    }
    std::uint64_t content_bytes=0;
    for(const auto part:parts) {
        if(part.size()>remaining)throw std::length_error("experience does not fit in block");
        remaining-=part.size();content_bytes+=part.size();
    }
    const auto total = capacity_ - end_ - remaining;
    StorageBudget::Reservation charge(storage_, total);
    std::array<std::byte, prefix_bytes> prefix{};
    std::memcpy(prefix.data(), record_magic.data(), record_magic.size());
    put_u64(prefix, 8, total);
    put_u64(prefix, 16, experience.sequence);
    put_u64(prefix, 24, experience.observed_at_ns);
    put_u64(prefix, 32, experience.session.size());
    put_u64(prefix, 40, experience.source.size());
    put_u64(prefix, 48, experience.media_type.size());
    put_u64(prefix, 56, content_bytes);
    Sha256 hash;
    hash.update(prefix);
    hash.update(experience.session);
    hash.update(experience.source);
    hash.update(experience.media_type);
    for(const auto part:parts)hash.update(part);
    const auto digest = hash.finish();
    std::array<std::byte, trailer_bytes> trailer{};
    std::copy(digest.begin(), digest.end(), trailer.begin());
    std::memcpy(trailer.data() + 32, end_magic.data(), end_magic.size());
    put_u64(trailer, 40, total);
    auto at = end_;
    charge.retain();
    try {
        const auto write_part = [&](std::span<const std::byte> part) {
            write_exact(fd_, part, at, storage_);
            at += part.size();
        };
        for (const auto part : {std::span<const std::byte>(prefix), as_bytes(experience.session),
                               as_bytes(experience.source), as_bytes(experience.media_type)})write_part(part);
        for(const auto part:parts)write_part(part);
        write_part(trailer);
        sync_data(fd_);
    } catch (...) {
        writable_ = false;
        throw;
    }
    const ExperienceLocation location{identity_, end_, total, digest};
    end_ = at;
    return location;
}

ExperienceLocation ExperienceBlock::location_at(std::uint64_t offset) const {
    if (fd_ < 0 || offset < header_bytes || offset > capacity_ ||
        capacity_ - offset < record_overhead)
        throw std::invalid_argument("invalid experience frame offset");
    const auto size = file_size(fd_);
    if (offset > size || size - offset < record_overhead)
        throw std::runtime_error("incomplete experience frame");
    std::array<std::byte, prefix_bytes> prefix;
    read_exact(fd_, prefix, offset, storage_);
    const auto total = record_length(prefix, capacity_ - offset);
    if (total > size - offset) throw std::runtime_error("incomplete experience frame");
    std::array<std::byte, trailer_bytes> trailer;
    read_exact(fd_, trailer, offset + total - trailer_bytes, storage_);
    DigestBytes digest;
    std::copy_n(trailer.begin(), digest.size(), digest.begin());
    check_trailer(trailer, total, digest);
    return {identity_, offset, total, digest};
}

StoredExperience ExperienceBlock::read(const ExperienceLocation& location,
    std::uint64_t max_read_bytes, MemoryBudget& memory) const {
    if (fd_ < 0 || location.block != identity_ || location.offset < header_bytes ||
        location.offset > capacity_ || location.bytes < record_overhead ||
        location.bytes > capacity_ - location.offset || location.bytes > max_read_bytes ||
        location.bytes > std::numeric_limits<std::size_t>::max())
        throw std::invalid_argument("invalid experience location or read budget");
    std::array<std::byte, prefix_bytes> prefix;
    read_exact(fd_, prefix, location.offset, storage_);
    if (record_length(prefix, capacity_ - location.offset) != location.bytes)
        throw std::runtime_error("experience location length mismatch");
    const auto size = file_size(fd_);
    if (location.offset > size || location.bytes > size - location.offset)
        throw std::runtime_error("incomplete selected experience");
    StoredExperience experience(memory);
    experience.encoded_.resize(static_cast<std::size_t>(location.bytes));
    std::copy(prefix.begin(), prefix.end(), experience.encoded_.begin());
    read_exact(fd_, std::span(experience.encoded_).subspan(prefix_bytes), location.offset + prefix_bytes, storage_);
    const auto encoded = std::span<const std::byte>(experience.encoded_);
    const auto digest = Sha256::of(encoded.first(encoded.size() - trailer_bytes));
    check_trailer(encoded.last(trailer_bytes), location.bytes, digest);
    if (digest != location.digest) throw std::runtime_error("selected experience digest mismatch");
    experience.location_ = location;
    return experience;
}

BlockRecovery ExperienceBlock::inspect() const {
    if (fd_ < 0) throw std::logic_error("closed experience block");
    const auto size = file_size(fd_);
    if (size < header_bytes || size > capacity_) throw std::runtime_error("invalid experience file size");
    BlockRecovery recovery{0, header_bytes, 0};
    std::array<std::byte, 65536> scratch;
    while (recovery.complete_bytes < size) {
        const auto at = recovery.complete_bytes;
        if (size - at < prefix_bytes) break;
        std::array<std::byte, prefix_bytes> prefix;
        read_exact(fd_, prefix, at, storage_);
        const auto bytes = record_length(prefix, capacity_ - at);
        if (bytes > size - at) break;
        Sha256 hash;
        hash.update(prefix);
        auto remaining = bytes - prefix_bytes - trailer_bytes;
        auto cursor = at + prefix_bytes;
        while (remaining > 0) {
            const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, scratch.size()));
            const auto chunk = std::span(scratch).first(count);
            read_exact(fd_, chunk, cursor, storage_);
            hash.update(chunk);
            cursor += count;
            remaining -= count;
        }
        std::array<std::byte, trailer_bytes> trailer;
        read_exact(fd_, trailer, cursor, storage_);
        const auto digest = hash.finish();
        check_trailer(trailer, bytes, digest);
        recovery.content_digest = extend_experience_digest(recovery.content_digest,
            {identity_, at, bytes, digest});
        recovery.complete_bytes += bytes;
        ++recovery.complete_records;
    }
    recovery.unfinished_bytes = size - recovery.complete_bytes;
    return recovery;
}

}  // namespace swegca::vrs
