#pragma once

#include "swegca_architecture/session_kernel.hpp"
#include "vrs/evidence_experience.hpp"

#include <map>
#include <optional>
#include <string>

namespace swegca::vrs {

// Main-owned, single writer per session. This retains originals and recorded
// observations; semantic graph/strength consolidation is a separate unfinished
// layer. Publication makes ended originals addressable under Main without
// moving their bytes. It does not promote their truth/authority.
class SessionStore final {
public:
    [[nodiscard]] static SessionStore create(const std::filesystem::path& root,
        const architecture::DigestBytes& identity, std::string_view name,
        std::uint64_t block_capacity, MemoryBudget& memory);
    [[nodiscard]] static SessionStore open(const std::filesystem::path& root,
        const architecture::DigestBytes& identity, MemoryBudget& memory);
    SessionStore(const SessionStore&) = delete;
    SessionStore& operator=(const SessionStore&) = delete;
    SessionStore(SessionStore&&) = delete;
    SessionStore& operator=(SessionStore&&) = delete;

    [[nodiscard]] ExperienceLocation append(const OriginalExperienceView& experience);
    [[nodiscard]] ExperienceEvidence append_evidence(const architecture::kernel::EvidenceRules& rules,
        const OriginalExperienceView& original, const architecture::kernel::EvidenceObservation& value);
    [[nodiscard]] StoredExperience read(const ExperienceLocation& location, std::uint64_t limit) const;
    void end();
    void publish_originals();

    [[nodiscard]] architecture::kernel::SessionPhase phase() const noexcept { return phase_; }
    [[nodiscard]] std::uint64_t original_count() const noexcept { return records_; }
    [[nodiscard]] std::size_t block_count() const noexcept { return blocks_.size(); }
    [[nodiscard]] bool usable() const noexcept { return usable_; }
    [[nodiscard]] std::string_view name() const noexcept { return name_; }

private:
    struct BlockState {
        std::uint64_t index = 0;
        BlockRecovery extent{0, ExperienceBlock::header_bytes, 0};
    };
    SessionStore(const std::filesystem::path& root, const architecture::DigestBytes& identity,
        MemoryBudget& memory, bool create, std::string_view name, std::uint64_t block_capacity);
    void require(architecture::kernel::SessionOperation operation) const;
    void next_block();
    void verify_name(const OriginalExperienceView& experience) const;
    void recorded(const ExperienceLocation& location) noexcept;
    [[nodiscard]] std::filesystem::path block_path(std::uint64_t index) const;
    [[nodiscard]] architecture::DigestBytes inventory() const;

    std::filesystem::path root_, directory_;
    architecture::DigestBytes identity_;
    MemoryBudget& memory_;
    std::pmr::string name_;
    std::uint64_t block_capacity_ = 0, next_block_ = 0, records_ = 0, current_records_ = 0;
    architecture::kernel::SessionPhase phase_ = architecture::kernel::SessionPhase::invalid;
    bool usable_ = true;
    std::pmr::map<architecture::DigestBytes, BlockState> blocks_;
    std::optional<ExperienceBlock> control_, writer_;
};

}  // namespace swegca::vrs
