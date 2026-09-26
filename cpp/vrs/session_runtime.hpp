#pragma once

#include "vrs/connection_catalog.hpp"
#include "swegca_architecture/recall_route_kernel.hpp"

namespace swegca::vrs {

struct RecordedRefinement {
    ExperienceLocation original;
    ConnectionRefinement refinement;
};

struct CueReference {
    architecture::DigestBytes connection;
    std::size_t original_index = 0;
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
    [[nodiscard]] RecordedRefinement observe(const architecture::DigestBytes& identity,
        const OriginalExperienceView& original, const architecture::kernel::EvidenceObservation& observation,
        std::uint64_t shuffle_seed, std::uint64_t current_step);
    // Retains natural content through the same shuffle/core path. With no
    // observed outcome, evidence stays insufficient. No prose verdict is made.
    [[nodiscard]] RecordedRefinement retain_input(const OriginalExperienceView& original,
        double initial_strength, const architecture::EvidencePolicy& policy,
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
    std::pmr::map<architecture::DigestBytes, std::pmr::vector<CueReference>> cues_;
    bool usable_ = true;
};

struct RecallMatch {
    const SessionRuntime* session = nullptr;
    const PersistentConnection* connection = nullptr;
    architecture::kernel::ConnectionHead recalled_head;
};

struct InputMatch {
    RecallMatch recalled;
    std::size_t original_index = 0;
};

class InputRecall final {
public:
    InputRecall(const InputRecall&) = delete;
    InputRecall& operator=(const InputRecall&) = delete;
    InputRecall(InputRecall&&) noexcept = default;
    InputRecall& operator=(InputRecall&&) = delete;
    [[nodiscard]] bool familiar() const noexcept { return !matches_.empty(); }
    [[nodiscard]] bool temporary() const noexcept { return temporary_; }
    [[nodiscard]] architecture::kernel::FamiliarityKey key_kind() const noexcept { return key_kind_; }
    [[nodiscard]] std::span<const InputMatch> matches() const noexcept { return matches_; }
    [[nodiscard]] const architecture::DigestBytes& cue() const noexcept { return cue_; }
private:
    friend class ExperienceRouter;
    explicit InputRecall(MemoryBudget& memory) : matches_(&memory) {}
    architecture::DigestBytes cue_{};
    bool temporary_ = false;
    architecture::kernel::FamiliarityKey key_kind_ = architecture::kernel::FamiliarityKey::missing;
    std::pmr::vector<InputMatch> matches_;
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
    // Immediate natural-input entry: exact/continued familiarity, then Recall.
    // No disk, recording, shuffle or LLM precedes Recall. SHA-256 cue work is
    // part of Deja vu and is included in any input-to-Recall timing.
    [[nodiscard]] InputRecall input(std::string_view media, std::span<const std::byte> content) const;
    [[nodiscard]] StoredExperience replay(const InputRecall& recalled, std::size_t candidate) const;
    [[nodiscard]] StoredExperience replay(const RecallCandidates& candidates, std::size_t candidate,
        std::size_t original_index) const;
private:
    SessionRuntime& temporary_;
    MemoryBudget& memory_;
    std::pmr::vector<const SessionRuntime*> mounted_;
    std::pmr::map<architecture::DigestBytes, std::pmr::vector<RecallMatch>> main_;
    struct MainCue { const SessionRuntime* session; CueReference reference; };
    std::pmr::map<architecture::DigestBytes, std::pmr::vector<MainCue>> main_cues_;
    // Main-owned dialogue continuity, updated only after a successful selected
    // Replay. It holds an experience key, never copied dialogue text or a verdict.
    mutable std::optional<architecture::DigestBytes> continuation_;
    [[nodiscard]] InputRecall recall_cue(const architecture::DigestBytes& cue,
        architecture::kernel::RecallScope scope, architecture::kernel::FamiliarityKey kind) const;
};

}  // namespace swegca::vrs
