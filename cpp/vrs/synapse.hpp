#pragma once

#include "vrs/persistent_connection.hpp"

namespace swegca::vrs {

// Main retains both addresses as one checkpoint. The immutable binding names
// the actual members and relation definition; the state names the core-checked
// append/refine journal. Neither a lookup cue nor a caller's verdict is a synapse.
struct SynapseCheckpoint {
    ExperienceLocation binding;
    ExperienceLocation state;
};

// Experience creation/maintenance only. No Recall/Replay route or runtime is
// called here. SessionStore is the underlying authenticated record storage.
class Synapse final {
public:
    // Member order is retained: position/role is part of the relation, not an
    // arbitrary undirected similarity edge. Members and definition must already
    // be sealed in this store. No synthetic relation meaning is inferred.
    static Synapse create(SessionStore&, std::span<const ExperienceLocation> members,
        const ExperienceLocation& definition, double initial_strength,
        const architecture::EvidencePolicy&, MemoryBudget&, std::uint64_t read_limit);
    static Synapse recover(SessionStore&, const SynapseCheckpoint&,
        MemoryBudget&, std::uint64_t read_limit);
    Synapse(const Synapse&) = delete;
    Synapse& operator=(const Synapse&) = delete;
    Synapse(Synapse&&) = delete;
    Synapse& operator=(Synapse&&) = delete;

    // This is a measured observation, not an accept/reject/abstain decision.
    // hypothesis must match identity(); no relabelling or default abstention.
    [[nodiscard]] ExperienceLocation observe(const OriginalExperienceView&,
        const architecture::kernel::EvidenceObservation&);
    void append(const ExperienceLocation& sealed_observation);
    [[nodiscard]] ConnectionRefinement refine(std::uint64_t seed, std::uint64_t step);

    [[nodiscard]] SynapseCheckpoint checkpoint() const noexcept { return {binding_.record, connection_.head()}; }
    [[nodiscard]] const architecture::DigestBytes& identity() const noexcept { return binding_.identity; }
    [[nodiscard]] std::span<const ExperienceLocation> members() const noexcept { return binding_.members; }
    [[nodiscard]] const ExperienceLocation& definition() const noexcept { return binding_.definition; }
    [[nodiscard]] double strength() const noexcept { return connection_.state().strength(); }
    [[nodiscard]] std::uint64_t revision() const noexcept { return connection_.state().revision(); }
    [[nodiscard]] const architecture::kernel::EvidenceRules& rules() const noexcept { return connection_.rules(); }

private:
    struct Binding {
        explicit Binding(MemoryBudget& memory) : members(&memory) {}
        ExperienceLocation record{}, definition{};
        architecture::DigestBytes identity{};
        std::pmr::vector<ExperienceLocation> members;
    };
    static Binding write_binding(SessionStore&, std::span<const ExperienceLocation>,
        const ExperienceLocation&, MemoryBudget&, std::uint64_t);
    static Binding read_binding(SessionStore&, const ExperienceLocation&, MemoryBudget&, std::uint64_t);
    Synapse(SessionStore&, Binding, double, const architecture::EvidencePolicy&, MemoryBudget&, std::uint64_t);
    Synapse(SessionStore&, Binding, const ExperienceLocation&, MemoryBudget&, std::uint64_t);
    SessionStore& session_;
    Binding binding_;
    PersistentConnection connection_;
};

} // namespace swegca::vrs
