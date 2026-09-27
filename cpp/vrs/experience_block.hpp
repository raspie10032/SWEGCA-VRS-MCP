#pragma once

#include "swegca_architecture/digest_bytes.hpp"
#include "swegca_architecture/record_address.hpp"
#include "vrs/memory_budget.hpp"
#include "vrs/storage_budget.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <optional>
#include <string_view>
#include <vector>

namespace swegca::architecture::kernel { class EvidenceRules; struct EvidenceObservation; }

namespace swegca::vrs {
class ExperienceEvidence;

// Physical original-experience storage owned by Main. No evidence verdict or
// mutation authority is inferred from storage/read success (SWEGCA I03/I07,
// ARCHITECTURE_SPEC.md@5901a5a sections 2, 4.2). Failed and unknown experience
// content is preserved byte-for-byte; the VRS layer binds evidence separately.
enum class ExperienceSender : std::uint8_t { unspecified, client, server };

struct OriginalExperienceView {
    std::uint64_t sequence = 0;
    std::uint64_t observed_at_ns = 0;
    std::string_view session;
    std::string_view source;
    std::string_view media_type;
    std::span<const std::byte> content;
    ExperienceSender sender = ExperienceSender::unspecified;
};

using ExperienceLocation = architecture::RecordAddress;

class StoredExperience final {
public:
    StoredExperience(const StoredExperience&) = delete;
    StoredExperience& operator=(const StoredExperience&) = delete;
    StoredExperience(StoredExperience&&) noexcept = default;
    StoredExperience& operator=(StoredExperience&&) = delete;

    [[nodiscard]] OriginalExperienceView view() const noexcept;
    [[nodiscard]] const ExperienceLocation& location() const noexcept { return location_; }

private:
    friend class ExperienceBlock;
    explicit StoredExperience(MemoryBudget& memory) : encoded_(&memory) {}
    ExperienceLocation location_;
    std::pmr::vector<std::byte> encoded_;
};

struct BlockRecovery {
    std::uint64_t complete_records = 0;
    std::uint64_t complete_bytes = 0;
    std::uint64_t unfinished_bytes = 0;
    architecture::DigestBytes content_digest{};
};

// Bind the ordered, fully committed record addresses without rereading their
// bodies. Recovery reconstructs the same chain after validating each record.
[[nodiscard]] architecture::DigestBytes extend_experience_digest(
    const architecture::DigestBytes& previous, const ExperienceLocation& location) noexcept;

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
        const architecture::DigestBytes& identity, std::uint64_t capacity, StorageBudget* storage = nullptr);
    [[nodiscard]] static ExperienceBlock open_reader(const std::filesystem::path& path, StorageBudget* storage = nullptr);
    [[nodiscard]] static ExperienceBlock open_writer(const std::filesystem::path& path, StorageBudget* storage = nullptr);

    ExperienceBlock(const ExperienceBlock&) = delete;
    ExperienceBlock& operator=(const ExperienceBlock&) = delete;
    ExperienceBlock(ExperienceBlock&& other) noexcept;
    ExperienceBlock& operator=(ExperienceBlock&& other) noexcept;
    ~ExperienceBlock();

    // Address is returned only after the complete record is fdatasync'ed.
    // On I/O failure this handle refuses further appends; partial bytes remain.
    [[nodiscard]] ExperienceLocation append(const OriginalExperienceView& experience);

    // Allocation uses Main's shared VRS budget as well as a per-read limit.
    // `memory` must outlive the returned record. Moving it does not allocate.
    // Only this record is read. No scan of other experiences occurs here.
    [[nodiscard]] StoredExperience read(const ExperienceLocation& location,
        std::uint64_t max_read_bytes, MemoryBudget& memory) const;

    // Cold-path framing lookup at a known record boundary (metadata/recovery).
    // read() verifies the returned record's content checksum. An incomplete
    // frame throws; normal Replay already has its address and never calls this.
    [[nodiscard]] ExperienceLocation location_at(std::uint64_t offset) const;

    // Startup recovery streams through a fixed-size scratch buffer. A short
    // final record is reported and preserved; a complete corrupt record throws.
    // Normal selected reads do not call this scan.
    [[nodiscard]] BlockRecovery inspect() const;

    [[nodiscard]] const architecture::DigestBytes& identity() const noexcept { return identity_; }
    [[nodiscard]] std::uint64_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] bool can_append() const noexcept { return writable_; }

private:
    ExperienceBlock() = default;
    // Only ExperiencePage may request cleanup of a newly created derived file.
    [[nodiscard]] static ExperienceBlock create_impl(const std::filesystem::path&,
        const architecture::DigestBytes&,std::uint64_t,StorageBudget*,bool derived);
    friend class EvidenceReader;
    friend class ExperiencePage;
    friend ExperienceEvidence record_evidence(ExperienceBlock&,
        const architecture::kernel::EvidenceRules&, const OriginalExperienceView&,
        const architecture::kernel::EvidenceObservation&, std::optional<architecture::DigestBytes>);
    friend ExperienceEvidence read_evidence(const architecture::kernel::EvidenceRules&,
        const ExperienceBlock&, const ExperienceLocation&, std::uint64_t);
    // Private decoder visitor. Chunks are provisional until this returns after
    // checking the complete record; the decoder may not publish them early.
    // Private callbacks: all four original fields and original sequence/time.
    [[nodiscard]] std::uint64_t visit_evidence(const ExperienceLocation&, std::uint64_t,
        void*, void (*)(void*, unsigned, std::span<const std::byte>, std::uint64_t, std::uint64_t),
        void (*)(void*, std::uint64_t, std::uint64_t)) const;
    [[nodiscard]] ExperienceLocation append_parts(const OriginalExperienceView&,
        std::span<const std::span<const std::byte>> content);
    [[nodiscard]] static ExperienceBlock open(const std::filesystem::path& path, bool writer, StorageBudget* storage);
    int fd_ = -1;
    architecture::DigestBytes identity_{};
    std::uint64_t capacity_ = 0;
    std::uint64_t end_ = header_bytes;
    bool writable_ = false;
    StorageBudget* storage_ = nullptr;
};

}  // namespace swegca::vrs
