#pragma once
#include "vrs/evidence_experience.hpp"

namespace swegca::vrs {
// Derived storage only: construction requires already sealed evidence values.
// A page handle carries the exact checksum address returned by its own write;
// there is no public path-only reopen or arbitrary metadata admission API.
class ExperiencePage final {
public:
    static constexpr std::size_t capacity = 256;
    [[nodiscard]] static ExperiencePage create(const std::filesystem::path&,
        const architecture::DigestBytes&, std::span<const ExperienceEvidence>,
        MemoryBudget&, StorageBudget* = nullptr);
    ExperiencePage(const ExperiencePage&)=delete;
    ExperiencePage& operator=(const ExperiencePage&)=delete;
    ExperiencePage(ExperiencePage&&) noexcept;
    ExperiencePage& operator=(ExperiencePage&&) noexcept;
    ~ExperiencePage();
    [[nodiscard]] std::size_t size() const noexcept{return count_;}
    [[nodiscard]] ExperienceEvidence read(std::size_t,
        const architecture::kernel::EvidenceRules&,MemoryBudget&) const;
    [[nodiscard]] std::pmr::vector<ExperienceEvidence> load(
        const architecture::kernel::EvidenceRules&,MemoryBudget&) const;
private:
    friend class ExperienceSequence;
    // Constructs into unpublished raw segment storage; destroys the prefix on
    // failure. No partially authenticated/decoded segment reaches a reader.
    void restore_into(ExperienceEvidence*,std::size_t,
        const architecture::kernel::EvidenceRules&,MemoryBudget&) const;
    [[nodiscard]] ExperienceEvidence decode(std::size_t,
        const architecture::kernel::EvidenceRules&,const StoredExperience&) const;
    void discard() noexcept;
    ExperiencePage(ExperienceBlock block,ExperienceLocation location,std::size_t count,
        std::filesystem::path path)
        :block_(std::move(block)),location_(location),count_(count),path_(std::move(path)){}
    ExperienceBlock block_;
    ExperienceLocation location_;
    std::size_t count_;
    std::filesystem::path path_;
};
} // namespace swegca::vrs
