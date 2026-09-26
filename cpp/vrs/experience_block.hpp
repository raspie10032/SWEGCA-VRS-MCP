#pragma once

#include "swegca_architecture/digest_bytes.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

namespace swegca::vrs {

// Physical original-experience storage owned by Main. No evidence verdict or
// mutation authority is inferred from storage/read success (SWEGCA I03/I07,
// ARCHITECTURE_SPEC.md@5901a5a sections 2, 4.2). Failed and unknown experience
// content is preserved byte-for-byte; the VRS layer binds evidence separately.
struct OriginalExperienceView {
    std::uint64_t sequence = 0;
    std::uint64_t observed_at_ns = 0;
    std::string_view session;
    std::string_view source;
    std::string_view media_type;
    std::span<const std::byte> content;
};

struct ExperienceLocation {
    architecture::DigestBytes block{};
    std::uint64_t offset = 0;
    std::uint64_t bytes = 0;
    architecture::DigestBytes digest{};
};

class StoredExperience final {
public:
    [[nodiscard]] OriginalExperienceView view() const noexcept;
    [[nodiscard]] const ExperienceLocation& location() const noexcept { return location_; }

private:
    friend class ExperienceBlock;
    ExperienceLocation location_;
    std::vector<std::byte> encoded_;
};

struct BlockRecovery {
    std::uint64_t complete_records = 0;
    std::uint64_t complete_bytes = 0;
    std::uint64_t unfinished_bytes = 0;
};

// One bounded block. Block identity and capacity are assigned by Main. Blocks
// can be owned/written independently by different workers; one block has one
// writer. Callers serialize access to a writer object; readers use own handles.
// A full or interrupted block is retained and Main continues in another block.
// This class never deletes/truncates originals, merges sessions, or chooses
// what experience Recall selects. Read a supplied address after that selection.
class ExperienceBlock final {
public:
    static constexpr std::uint64_t header_bytes = 80;
    static constexpr std::uint64_t record_overhead = 112;

    [[nodiscard]] static ExperienceBlock create(const std::filesystem::path& path,
        const architecture::DigestBytes& identity, std::uint64_t capacity);
    [[nodiscard]] static ExperienceBlock open_reader(const std::filesystem::path& path);
    [[nodiscard]] static ExperienceBlock open_writer(const std::filesystem::path& path);

    ExperienceBlock(const ExperienceBlock&) = delete;
    ExperienceBlock& operator=(const ExperienceBlock&) = delete;
    ExperienceBlock(ExperienceBlock&& other) noexcept;
    ExperienceBlock& operator=(ExperienceBlock&& other) noexcept;
    ~ExperienceBlock();

    // Address is returned only after the complete record is fdatasync'ed.
    // On I/O failure this handle refuses further appends; partial bytes remain.
    [[nodiscard]] ExperienceLocation append(const OriginalExperienceView& experience);

    // Allocation is bounded by the caller's VRS read budget, not a core limit.
    // Only this record is read. No scan of other experiences occurs here.
    [[nodiscard]] StoredExperience read(const ExperienceLocation& location,
        std::uint64_t max_read_bytes) const;

    // Startup recovery streams through a fixed-size scratch buffer. A short
    // final record is reported and preserved; a complete corrupt record throws.
    // Normal selected reads do not call this scan.
    [[nodiscard]] BlockRecovery inspect() const;

    [[nodiscard]] const architecture::DigestBytes& identity() const noexcept { return identity_; }
    [[nodiscard]] std::uint64_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] bool can_append() const noexcept { return writable_; }

private:
    ExperienceBlock() = default;
    [[nodiscard]] static ExperienceBlock open(const std::filesystem::path& path, bool writer);
    int fd_ = -1;
    architecture::DigestBytes identity_{};
    std::uint64_t capacity_ = 0;
    std::uint64_t end_ = header_bytes;
    bool writable_ = false;
};

}  // namespace swegca::vrs
