#pragma once

#include "swegca_architecture/evidence_rules.hpp"
#include "swegca_architecture/head_publication_kernel.hpp"
#include "vrs/connection.hpp"
#include "vrs/session_store.hpp"

#include <optional>

namespace swegca::vrs {

// Main owns the authoritative head address and one serialized connection.
// Records are immutable and parent-linked; recovery starts from that supplied
// head rather than choosing the last matching text/log entry. Publishing Main's
// graph/root head is a separate remaining layer.
class PersistentConnection final {
public:
    [[nodiscard]] static PersistentConnection create(SessionStore& session,
        const architecture::DigestBytes& identity, double initial_strength,
        const architecture::EvidencePolicy& policy, MemoryBudget& memory, std::uint64_t original_read_limit);
    [[nodiscard]] static PersistentConnection recover(SessionStore& session,
        const ExperienceLocation& head, MemoryBudget& memory, std::uint64_t original_read_limit);
    PersistentConnection(const PersistentConnection&) = delete;
    PersistentConnection& operator=(const PersistentConnection&) = delete;
    PersistentConnection(PersistentConnection&&) = delete;
    PersistentConnection& operator=(PersistentConnection&&) = delete;

    // Read the address's actual recorded observation before using it. No raw
    // strength/tally setter exists. These calls publish to the live session only.
    void append(const ExperienceLocation& original);
    [[nodiscard]] ConnectionRefinement refine(std::uint64_t seed, std::uint64_t current_step);
    [[nodiscard]] const ExperienceLocation& head() const noexcept { return head_; }
    [[nodiscard]] const Connection& state() const noexcept { return *state_; }
    [[nodiscard]] const architecture::EvidencePolicy& policy() const noexcept { return policy_; }
    [[nodiscard]] const architecture::kernel::EvidenceRules& rules() const noexcept { return *rules_; }
    [[nodiscard]] architecture::kernel::ConnectionHead snapshot() const noexcept;
    // Recover an ancestor boundary from this already core-verified owner.
    // Reads connection records only; never trusts caller-supplied counts/strength.
    [[nodiscard]] architecture::kernel::ConnectionHead historical_snapshot(const ExperienceLocation&) const;
    [[nodiscard]] std::pair<std::uint64_t,std::uint64_t> latest_refinement_parameters() const;
    // Addresses are ordered newest to oldest. Verification reads only the
    // linked connection records and never treats a greater number as ancestry.
    [[nodiscard]] bool contains_history(std::span<const ExperienceLocation> addresses) const;
    [[nodiscard]] static bool verifies_extension(SessionStore& session,
        const architecture::kernel::ConnectionHead& candidate, const ExperienceLocation& previous);

private:
    friend class ConnectionCatalog;
    PersistentConnection(SessionStore&, const architecture::DigestBytes&, double,
        const architecture::EvidencePolicy&, MemoryBudget&, std::uint64_t);
    PersistentConnection(SessionStore&, const ExperienceLocation&, MemoryBudget&, std::uint64_t);
    SessionStore& session_;
    MemoryBudget& memory_;
    std::uint64_t original_read_limit_;
    architecture::EvidencePolicy policy_;
    std::optional<architecture::kernel::EvidenceRules> rules_;
    std::optional<Connection> state_;
    ExperienceLocation head_;
    std::uint64_t ordinal_ = 0;
};

}  // namespace swegca::vrs
