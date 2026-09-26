#pragma once

#include "vrs/connection_catalog.hpp"
#include <set>
#include <iterator>
#include "swegca_architecture/recall_route_kernel.hpp"
#include "swegca_architecture/replay_evidence_kernel.hpp"

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
    friend class MainGraph;
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

class PersistentMainGraph;

struct RecallMatch {
    const SessionRuntime* session = nullptr;
    const PersistentConnection* connection = nullptr;
    architecture::kernel::ConnectionHead recalled_head;
    const PersistentMainGraph* main_graph = nullptr;
};

struct InputMatch {
    RecallMatch recalled;
    std::size_t original_index = 0;
    std::size_t current_observations = 0;
    ExperienceLocation original;
};

class ExperienceRouter;

// Owns pinned original addresses and one remembered head per consecutive
// connection group. Matches are assembled by value without reading live state.
class InputRecall final {
public:
    class View final {
    public:
        class Iterator final {
        public:
            using value_type = InputMatch;
            using difference_type = std::ptrdiff_t;
            using iterator_category = std::input_iterator_tag;
            Iterator() = default;
            [[nodiscard]] InputMatch operator*() const { return owner_->at(index_); }
            Iterator& operator++() { ++index_; return *this; }
            Iterator operator++(int) { auto previous = *this; ++*this; return previous; }
            bool operator==(const Iterator&) const = default;
        private:
            friend class View;
            Iterator(const InputRecall* owner, std::size_t index) : owner_(owner), index_(index) {}
            const InputRecall* owner_ = nullptr;
            std::size_t index_ = 0;
        };
        [[nodiscard]] std::size_t size() const noexcept { return owner_->count_; }
        [[nodiscard]] bool empty() const noexcept { return !size(); }
        [[nodiscard]] InputMatch operator[](std::size_t index) const { return owner_->at(index); }
        [[nodiscard]] Iterator begin() const noexcept { return Iterator(owner_, 0); }
        [[nodiscard]] Iterator end() const noexcept { return Iterator(owner_, size()); }
    private:
        friend class InputRecall;
        explicit View(const InputRecall* owner) : owner_(owner) {}
        const InputRecall* owner_;
    };
    InputRecall(const InputRecall&) = delete;
    InputRecall& operator=(const InputRecall&) = delete;
    InputRecall(InputRecall&& other) noexcept
        : cue_(other.cue_), issuer_(std::exchange(other.issuer_, nullptr)), temporary_(other.temporary_),
          key_kind_(other.key_kind_), count_(std::exchange(other.count_, 0)),
          contexts_(std::move(other.contexts_)), addresses_(std::move(other.addresses_)) {}
    InputRecall& operator=(InputRecall&&) = delete;
    [[nodiscard]] bool familiar() const noexcept { return count_ != 0; }
    [[nodiscard]] bool temporary() const noexcept { return temporary_; }
    [[nodiscard]] architecture::kernel::FamiliarityKey key_kind() const noexcept { return key_kind_; }
    // The receipt must outlive its view. No reference to a synthesized match is retained.
    [[nodiscard]] View matches() const noexcept { return View(this); }
    [[nodiscard]] const architecture::DigestBytes& cue() const noexcept { return cue_; }
private:
    friend class ExperienceRouter;
    struct Context {
        RecallMatch recalled;
        std::size_t current_observations;
        std::optional<ExperienceSequence::Snapshot> sequence;
        std::size_t end = 0;
    };
    struct Address { std::size_t context; std::size_t original_index; std::shared_ptr<const ExperienceEvidence> experience; };
    explicit InputRecall(MemoryBudget& memory) : contexts_(&memory), addresses_(&memory) {}
    void append(const RecallMatch&, std::size_t index, std::size_t boundary, std::shared_ptr<const ExperienceEvidence>);
    void append_range(const RecallMatch&, std::size_t boundary, const Connection&, MemoryBudget&);
    [[nodiscard]] InputMatch at(std::size_t index) const {
        if (index >= count_) throw std::out_of_range("input recall candidate");
        if (key_kind_ == architecture::kernel::FamiliarityKey::continuation) {
            const auto found = std::upper_bound(contexts_.begin(), contexts_.end(), index,
                [](std::size_t position, const Context& range) { return position < range.end; });
            const auto original_index = index - (found->end - found->sequence->size());
            return {found->recalled, original_index, found->current_observations,
                (*found->sequence)[original_index].original()};
        }
        const auto& address = addresses_.at(index);
        const auto& context = contexts_[address.context];
        return {context.recalled, address.original_index, context.current_observations, address.experience->original()};
    }
    architecture::DigestBytes cue_{};
    const ExperienceRouter* issuer_ = nullptr;
    bool temporary_ = false;
    architecture::kernel::FamiliarityKey key_kind_ = architecture::kernel::FamiliarityKey::missing;
    std::size_t count_ = 0;
    std::pmr::vector<Context> contexts_;
    std::pmr::vector<Address> addresses_;
};

// Issued only after a successful selected original read. No caller can supply
// a made-up prior outcome/head or manufacture a pre-Replay receipt.
class ReplayedInput final {
public:
    ReplayedInput(const ReplayedInput&) = delete;
    ReplayedInput& operator=(const ReplayedInput&) = delete;
    ReplayedInput(ReplayedInput&&) noexcept = default;
    [[nodiscard]] const StoredExperience& original() const noexcept { return original_; }
    [[nodiscard]] const ExperienceLocation& location() const noexcept { return original_.location(); }
    [[nodiscard]] const architecture::DigestBytes& input_cue() const noexcept { return input_cue_; }
private:
    friend class ExperienceRouter;
    ReplayedInput(StoredExperience original, InputMatch match, const ExperienceRouter* issuer,
        architecture::DigestBytes cue)
        : original_(std::move(original)), match_(match), issuer_(issuer), input_cue_(cue) {}
    StoredExperience original_;
    InputMatch match_;
    const ExperienceRouter* issuer_;
    architecture::DigestBytes input_cue_;
};

class ReEvidenceResult final {
public:
    ReEvidenceResult(const ReEvidenceResult&) = delete;
    ReEvidenceResult& operator=(const ReEvidenceResult&) = delete;
    ReEvidenceResult(ReEvidenceResult&&) noexcept = default;
    [[nodiscard]] const ConnectionRefinement& verification() const noexcept { return verification_; }
    [[nodiscard]] architecture::kernel::ReplayAgreement agreement() const noexcept { return agreement_; }
    [[nodiscard]] std::span<const ExperienceLocation> current_originals() const noexcept { return current_originals_; }
    [[nodiscard]] const architecture::kernel::ConnectionHead& remembered_head() const noexcept { return remembered_; }
    [[nodiscard]] const architecture::kernel::ConnectionHead& current_head() const noexcept { return current_; }
    [[nodiscard]] const ExperienceLocation& replayed_original() const noexcept { return replayed_original_; }
    [[nodiscard]] const architecture::DigestBytes& input_cue() const noexcept { return input_cue_; }
private:
    friend class ExperienceRouter;
    ReEvidenceResult(ConnectionRefinement report, architecture::kernel::ReplayAgreement agreement,
        architecture::kernel::ConnectionHead remembered, architecture::kernel::ConnectionHead current,
        ExperienceLocation replayed_original, architecture::DigestBytes cue, std::pmr::vector<ExperienceLocation> originals)
        : verification_(std::move(report)), agreement_(agreement), remembered_(remembered), current_(current),
          replayed_original_(replayed_original), input_cue_(cue), current_originals_(std::move(originals)) {}
    ConnectionRefinement verification_;
    architecture::kernel::ReplayAgreement agreement_;
    architecture::kernel::ConnectionHead remembered_, current_;
    ExperienceLocation replayed_original_;
    architecture::DigestBytes input_cue_;
    std::pmr::vector<ExperienceLocation> current_originals_;
};

// Borrowed result: use before changing the router or its sessions. No original
// bytes are read by Recall. Main's caller selects one experience for Replay.
class RecallCandidates final {
public:
    [[nodiscard]] std::size_t size() const noexcept { return (temporary_.session || temporary_.main_graph) ? 1 : main_.size(); }
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
    // Atomically refresh the query index of one durable merged Main. Sources
    // need not retain their SessionRuntime caches. Graph/store owners outlive us.
    void mount_main(const PersistentMainGraph& graph);
    [[nodiscard]] RecallCandidates recall(const architecture::DigestBytes& identity) const;
    // Immediate natural-input entry: exact/continued familiarity, then Recall.
    // No disk, recording, shuffle or LLM precedes Recall. SHA-256 cue work is
    // part of Deja vu and is included in any input-to-Recall timing.
    [[nodiscard]] InputRecall input(std::string_view media, std::span<const std::byte> content) const;
    [[nodiscard]] ReplayedInput replay(const InputRecall& recalled, std::size_t candidate) const;
    // Verify only observations appended to this temporary session after Recall.
    // This is a read-only evaluation; it neither writes a second strength update
    // nor makes recalled originals count as new observations.
    [[nodiscard]] ReEvidenceResult re_evidence(const ReplayedInput& replayed,
        std::uint64_t shuffle_seed, std::uint64_t current_step) const;
    [[nodiscard]] StoredExperience replay(const RecallCandidates& candidates, std::size_t candidate,
        std::size_t original_index) const;
private:
    SessionRuntime& temporary_;
    MemoryBudget& memory_;
    std::pmr::vector<const SessionRuntime*> mounted_;
    std::pmr::map<architecture::DigestBytes, std::pmr::vector<RecallMatch>> main_;
    struct MainCue { const SessionRuntime* session; CueReference reference; };
    const PersistentMainGraph* merged_main_ = nullptr;
    ExperienceLocation merged_head_;
    std::uint64_t indexed_generation_ = 0;
    void require_main_current() const;
    [[nodiscard]] RecallMatch merged_match(const architecture::DigestBytes& identity) const;
    std::pmr::map<architecture::DigestBytes, std::pmr::vector<MainCue>> main_cues_;
    using MergedCueReference = std::pair<architecture::DigestBytes, std::size_t>;
    std::pmr::map<architecture::DigestBytes, std::pmr::set<MergedCueReference>> merged_cues_;
    // Main-owned dialogue continuity, updated only after a successful selected
    // Replay. It holds an experience key, never copied dialogue text or a verdict.
    mutable std::optional<architecture::DigestBytes> continuation_;
    [[nodiscard]] InputRecall recall_cue(const architecture::DigestBytes& cue,
        architecture::kernel::RecallScope scope, architecture::kernel::FamiliarityKey kind) const;
};

}  // namespace swegca::vrs
