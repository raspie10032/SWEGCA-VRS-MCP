#pragma once

#include "vrs/connection_catalog.hpp"
#include "swegca_architecture/recall_route_kernel.hpp"

namespace swegca::vrs {

struct RecordedRefinement {
    ExperienceLocation original;
    ConnectionRefinement refinement;
};

// Main owns this composition and serializes access. Store and budget outlive it.
// Reopen restores verified connections before admitting queries, not in Recall.
class SessionRuntime final {
public:
    SessionRuntime(SessionStore& store, MemoryBudget& memory, std::uint64_t read_limit);
    SessionRuntime(const SessionRuntime&) = delete;
    SessionRuntime& operator=(const SessionRuntime&) = delete;
    void define_connection(const architecture::DigestBytes& identity, double strength,
        const architecture::EvidencePolicy& policy);
    [[nodiscard]] ExperienceLocation record(const OriginalExperienceView& original);
    [[nodiscard]] RecordedRefinement observe(const architecture::DigestBytes& identity,
        const OriginalExperienceView& original, const architecture::kernel::EvidenceObservation& observation,
        std::uint64_t shuffle_seed, std::uint64_t current_step);
    [[nodiscard]] const PersistentConnection* find(const architecture::DigestBytes& identity) const;
    [[nodiscard]] StoredExperience replay(const architecture::DigestBytes& identity, std::size_t original_index) const;
    void end();
    void publish_originals();
    [[nodiscard]] bool usable() const noexcept { return usable_ && store_.usable() && catalog_.usable(); }
    [[nodiscard]] architecture::kernel::SessionPhase phase() const noexcept { return store_.phase(); }

private:
    friend class ExperienceRouter;
    struct Slot {
        Slot(SessionStore&, MemoryBudget&, std::uint64_t, const ExperienceLocation&);
        Slot(SessionStore&, MemoryBudget&, std::uint64_t, const architecture::DigestBytes&,
            double, const architecture::EvidencePolicy&);
        ~Slot();
        Slot(const Slot&) = delete;
        Slot& operator=(const Slot&) = delete;
        MemoryBudget& memory;
        PersistentConnection* connection;
    };
    void require_usable() const;
    SessionStore& store_;
    MemoryBudget& memory_;
    std::uint64_t read_limit_;
    ConnectionCatalog catalog_;
    std::pmr::map<architecture::DigestBytes, Slot> connections_;
    bool usable_ = true;
};

struct RecallMatch {
    const SessionRuntime* session = nullptr;
    const PersistentConnection* connection = nullptr;
    architecture::kernel::ConnectionHead recalled_head;
};

// Borrowed result: use before changing the router or its sessions. No original
// bytes are read by Recall. Main's caller selects one experience for Replay.
class RecallCandidates final {
public:
    [[nodiscard]] std::size_t size() const noexcept { return temporary_.session ? 1 : main_.size(); }
    [[nodiscard]] RecallMatch at(std::size_t index) const;
    [[nodiscard]] bool temporary() const noexcept { return temporary_.session != nullptr; }
private:
    friend class ExperienceRouter;
    RecallMatch temporary_;
    std::span<const RecallMatch> main_;
};

// Session runtimes outlive this router. Main sessions are mounted only after
// publication. Distinct session lineages remain distinct candidates.
class ExperienceRouter final {
public:
    ExperienceRouter(SessionRuntime& temporary, MemoryBudget& memory);
    void mount_main(const SessionRuntime& session);
    [[nodiscard]] RecallCandidates recall(const architecture::DigestBytes& identity) const;
    [[nodiscard]] StoredExperience replay(const RecallCandidates& candidates, std::size_t candidate,
        std::size_t original_index) const;
private:
    SessionRuntime& temporary_;
    std::pmr::vector<const SessionRuntime*> mounted_;
    std::pmr::map<architecture::DigestBytes, std::pmr::vector<RecallMatch>> main_;
};

}  // namespace swegca::vrs
