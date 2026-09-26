#pragma once

#include "vrs/evidence_experience.hpp"
#include "vrs/verification.hpp"

#include <vector>

namespace swegca::vrs {

struct RefinementSample {
    std::uint32_t experience_index = 0;
    architecture::kernel::ObservationUse use = architecture::kernel::ObservationUse::invalid;
};

// Owns the complete shuffled traversal and per-experience admission outcomes.
// It grants no World/memory/action authority. Its indices refer to the owner's
// append-only experience sequence at before_revision().
class ConnectionRefinement final {
public:
    ConnectionRefinement(const ConnectionRefinement&) = delete;
    ConnectionRefinement& operator=(const ConnectionRefinement&) = delete;
    ConnectionRefinement(ConnectionRefinement&&) noexcept = default;
    ConnectionRefinement& operator=(ConnectionRefinement&&) = delete;
    [[nodiscard]] const architecture::DigestBytes& connection() const noexcept { return connection_; }
    [[nodiscard]] std::uint64_t before_revision() const noexcept { return before_revision_; }
    [[nodiscard]] std::uint64_t after_revision() const noexcept { return after_revision_; }
    [[nodiscard]] std::uint64_t seed() const noexcept { return seed_; }
    [[nodiscard]] std::uint64_t current_step() const noexcept { return current_step_; }
    [[nodiscard]] const architecture::kernel::EvidenceTally& evidence() const noexcept { return evidence_; }
    [[nodiscard]] const ConnectionVerification& result() const noexcept { return result_; }
    [[nodiscard]] std::span<const RefinementSample> samples() const noexcept { return samples_; }

private:
    friend class Connection;
    explicit ConnectionRefinement(MemoryBudget& memory) : samples_(&memory) {}
    architecture::DigestBytes connection_{};
    std::uint64_t before_revision_ = 0, after_revision_ = 0, seed_ = 0, current_step_ = 0;
    architecture::kernel::EvidenceTally evidence_;
    ConnectionVerification result_;
    std::pmr::vector<RefinementSample> samples_;
};

// Main owns and serializes each connection. Workers may own different
// connections; this object cannot be copied/moved into a second state owner.
// No precomputed tally or externally produced verdict can be installed here.
// Original records may be in different physical blocks. This stores their
// addresses and recorded observation values, never an invented text verdict.
class Connection final {
public:
    Connection(const architecture::DigestBytes& identity, double initial_strength,
        const architecture::kernel::EvidenceRules& rules, MemoryBudget& memory);
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    Connection(Connection&&) = delete;
    Connection& operator=(Connection&&) = delete;

    void append(const ExperienceEvidence& experience);
    [[nodiscard]] ConnectionRefinement refine(std::uint64_t seed, std::uint64_t current_step);
    [[nodiscard]] const architecture::DigestBytes& identity() const noexcept { return identity_; }
    [[nodiscard]] double strength() const noexcept { return strength_; }
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
    [[nodiscard]] std::span<const ExperienceEvidence> experiences() const noexcept { return experiences_; }

private:
    friend class PersistentConnection;
    void prepare_append(const ExperienceEvidence& experience);
    void commit_append(const ExperienceEvidence& experience) noexcept;
    [[nodiscard]] ConnectionRefinement prepare_refinement(std::uint64_t seed, std::uint64_t current_step) const;
    void commit_refinement(const ConnectionRefinement& report) noexcept;
    architecture::DigestBytes identity_;
    double strength_;
    std::uint64_t revision_ = 0;
    architecture::kernel::EvidenceRules rules_;
    MemoryBudget& memory_;
    std::pmr::vector<ExperienceEvidence> experiences_;
};

}  // namespace swegca::vrs
