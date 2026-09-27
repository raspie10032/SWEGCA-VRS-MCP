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
    // Copied from the admitted sealed observation, never reconstructed by transport.
    architecture::DigestBytes context;
};

// Coordinates from a sealed cognition record, never a stored verdict.
struct ReplayRecovery {
    bool temporary=false,seed_only=false;
    architecture::kernel::FamiliarityKey key_kind=architecture::kernel::FamiliarityKey::missing;
    architecture::DigestBytes input_cue{},lookup_key{},connection{},source{};
    ExperienceLocation remembered_head{},observation_head{},original{};
    std::size_t original_index=0,observation_boundary=0;
};

struct CueReference {
    architecture::DigestBytes connection;
    std::size_t original_index = 0;
};

// Main owns this composition and serializes access. Store and budget outlive it.
// Reopen restores verified connections before admitting queries, not in Recall.
class SessionRuntime final {
public:
    SessionRuntime(SessionStore& store, MemoryBudget& memory, std::uint64_t read_limit, std::uint32_t recovery_workers = 1);
    SessionRuntime(const SessionRuntime&) = delete;
    SessionRuntime& operator=(const SessionRuntime&) = delete;
    void define_connection(const architecture::DigestBytes& identity, double strength,
        const architecture::EvidencePolicy& policy);
    void ensure_connection(const architecture::DigestBytes& identity, double strength,
        const architecture::EvidencePolicy& policy);
    [[nodiscard]] RecordedRefinement observe(const architecture::DigestBytes& identity,
        const OriginalExperienceView& original, const architecture::kernel::EvidenceObservation& observation,
        std::uint64_t shuffle_seed, std::uint64_t current_step,
        std::optional<architecture::DigestBytes> input_key = std::nullopt);
    // Retains natural content through the same shuffle/core path. With no
    // observed outcome, evidence stays insufficient. No prose verdict is made.
    [[nodiscard]] RecordedRefinement retain_input(const OriginalExperienceView& original,
        double initial_strength, const architecture::EvidencePolicy& policy,
        std::uint64_t shuffle_seed, std::uint64_t current_step,
        std::optional<architecture::DigestBytes> input_key = std::nullopt);
    [[nodiscard]] const PersistentConnection* find(const architecture::DigestBytes& identity) const;
    [[nodiscard]] StoredExperience replay(const architecture::DigestBytes& identity, std::size_t original_index) const;
    [[nodiscard]] EvidencePayloadSlice read_payload_slice(const architecture::DigestBytes&, std::size_t original_index,
        std::uint64_t offset, std::uint64_t count) const;
    // Iterate only originals in committed connection heads. Startup/recovery
    // operation, never part of input or Recall. Each borrowed record is verified.
    void visit_deliveries(std::string_view session,std::string_view source,std::string_view media,
        void*,void (*)(void*,const OriginalDelivery&)) const;
    [[nodiscard]] StoredExperience read_original(const ExperienceLocation&) const;
    void save_cognition(const ExperienceLocation& input,std::span<const std::byte> metadata) {
        require_usable();store_.save_cognition(input,metadata);
    }
    [[nodiscard]] std::optional<StoredExperience> read_cognition(const ExperienceLocation& input) const {
        require_usable();return store_.read_cognition(input);
    }
    [[nodiscard]] architecture::DigestBytes save_cognition_revision(const ExperienceLocation& input,
        std::span<const std::byte> metadata,std::optional<architecture::DigestBytes> channel={}) {
        require_usable();return store_.save_cognition_revision(input,metadata,channel);
    }
    [[nodiscard]] std::optional<StoredExperience> read_latest_cognition(const ExperienceLocation& input,
        std::optional<architecture::DigestBytes> channel={}) const {
        require_usable();return store_.read_latest_cognition(input,channel);
    }
    void end();
    void publish_originals();
    [[nodiscard]] bool usable() const noexcept { return usable_ && store_.usable() && catalog_.usable(); }
    [[nodiscard]] architecture::kernel::SessionPhase phase() const noexcept { return store_.phase(); }

private:
    friend class ExperienceRouter;
    friend class MainGraph;
    struct Slot {
        explicit Slot(MemoryBudget& budget) : memory(budget), connection(nullptr) {}
        void recover(SessionStore&, std::uint64_t, const ExperienceLocation&);
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
    // Derived only from sealed observations, not similarity or inferred truth.
    std::pmr::map<architecture::DigestBytes, std::pmr::vector<CueReference>> contexts_;
    bool usable_ = true;
    const architecture::DigestBytes dialogue_context_;
    bool has_dialogue_input_ = false;
};

class PersistentMainGraph;

struct RecallMatch {
    const SessionRuntime* session = nullptr;
    const PersistentConnection* connection = nullptr;
    architecture::kernel::ConnectionHead recalled_head;
    const PersistentMainGraph* main_graph = nullptr;
    ExperienceLocation observation_head{};
};

struct InputMatch {
    RecallMatch recalled;
    std::size_t original_index = 0;
    std::size_t current_observations = 0;
    ExperienceLocation original;
    std::uint64_t observed_at = 0;
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
        : cue_(other.cue_), lookup_key_(other.lookup_key_), seed_only_(other.seed_only_),
          issuer_(std::exchange(other.issuer_, nullptr)), temporary_(other.temporary_),
          key_kind_(other.key_kind_), count_(std::exchange(other.count_, 0)),
          contexts_(std::move(other.contexts_)), addresses_(std::move(other.addresses_)) {}
    InputRecall& operator=(InputRecall&&) = delete;
    [[nodiscard]] bool familiar() const noexcept { return count_ != 0; }
    [[nodiscard]] bool temporary() const noexcept { return temporary_; }
    [[nodiscard]] architecture::kernel::FamiliarityKey key_kind() const noexcept { return key_kind_; }
    [[nodiscard]] const architecture::DigestBytes& lookup_key() const noexcept { return lookup_key_; }
    [[nodiscard]] bool seed_only() const noexcept { return seed_only_; }
    // The receipt must outlive its view. No reference to a synthesized match is retained.
    [[nodiscard]] View matches() const noexcept { return View(this); }
    [[nodiscard]] const architecture::DigestBytes& cue() const noexcept { return cue_; }
private:
    friend class ExperienceRouter;
    struct Context {
        RecallMatch recalled;
        std::size_t current_observations;
        std::optional<ExperienceSequence::Snapshot> sequence;
        std::size_t begin = 0,end = 0,address_begin = 0;
    };
    struct Address { std::size_t original_index; std::shared_ptr<const ExperienceEvidence> experience; };
    explicit InputRecall(MemoryBudget& memory) : contexts_(&memory), addresses_(&memory) {}
    void append(const RecallMatch&, std::size_t index, std::size_t boundary, std::shared_ptr<const ExperienceEvidence>);
    void append_range(const RecallMatch&, std::size_t boundary, const Connection&, MemoryBudget&,
        std::size_t begin, std::size_t end);
    [[nodiscard]] InputMatch at(std::size_t index) const {
        if (index >= count_) throw std::out_of_range("input recall candidate");
        const auto found = std::upper_bound(contexts_.begin(), contexts_.end(), index,
            [](std::size_t position, const Context& range) { return position < range.end; });
        const auto relative=index-found->begin;
        if (found->sequence) {
            const auto experience=found->sequence->read(relative);
            return {found->recalled, found->sequence->original_begin()+relative, found->current_observations,
                experience.original(), experience.value().observed_at};
        }
        const auto& address = addresses_.at(found->address_begin+relative);
        return {found->recalled, address.original_index, found->current_observations, address.experience->original(), address.experience->value().observed_at};
    }
    architecture::DigestBytes cue_{};
    architecture::DigestBytes lookup_key_{};
    bool seed_only_ = false;
    std::shared_ptr<const std::byte> issuer_;
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
    [[nodiscard]] const architecture::DigestBytes& source_identity() const noexcept { return source_identity_; }
    [[nodiscard]] std::size_t original_index() const noexcept { return match_.original_index; }
    [[nodiscard]] const ExperienceLocation& observation_head() const noexcept {
        return match_.recalled.main_graph?match_.recalled.observation_head:match_.recalled.recalled_head.record;
    }
    [[nodiscard]] const ExperienceLocation& location() const noexcept { return original_.location(); }
    [[nodiscard]] const architecture::DigestBytes& input_cue() const noexcept { return input_cue_; }
private:
    friend class ExperienceRouter;
    ReplayedInput(StoredExperience original, InputMatch match, std::shared_ptr<const std::byte> issuer,
        architecture::DigestBytes cue, architecture::DigestBytes source_identity)
        : original_(std::move(original)), match_(match), issuer_(std::move(issuer)), input_cue_(cue), source_identity_(source_identity) {}
    StoredExperience original_;
    InputMatch match_;
    std::shared_ptr<const std::byte> issuer_;
    architecture::DigestBytes input_cue_;
    architecture::DigestBytes source_identity_;
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

// A route-issued comparison of the Replay with new, sealed observations.
// It is not a re-evidence result or authority to modify a connection.
class ReplayComparison final {
public:
    ReplayComparison(const ReplayComparison&) = delete;
    ReplayComparison& operator=(const ReplayComparison&) = delete;
    ReplayComparison(ReplayComparison&&) noexcept = default;
    [[nodiscard]] const ReEvidenceResult& evidence() const noexcept { return evidence_; }
    [[nodiscard]] architecture::kernel::ReplayAgreement agreement() const noexcept { return evidence_.agreement(); }
    [[nodiscard]] std::size_t observation_boundary() const noexcept { return boundary_; }
private:
    friend class ExperienceRouter;
    ReplayComparison(ReEvidenceResult evidence, std::shared_ptr<const std::byte> issuer, std::size_t boundary)
        : evidence_(std::move(evidence)), issuer_(std::move(issuer)), boundary_(boundary) {}
    ReEvidenceResult evidence_;
    std::shared_ptr<const std::byte> issuer_;
    std::size_t boundary_;
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
    ExperienceRouter(const ExperienceRouter&) = delete;
    ExperienceRouter& operator=(const ExperienceRouter&) = delete;
    ExperienceRouter(ExperienceRouter&&) = delete;
    ExperienceRouter& operator=(ExperienceRouter&&) = delete;
    void mount_main(const SessionRuntime& session);
    // Atomically refresh the query index of one durable merged Main. Sources
    // need not retain their SessionRuntime caches. Graph/store owners outlive us.
    void mount_main(const PersistentMainGraph& graph);
    [[nodiscard]] RecallCandidates recall(const architecture::DigestBytes& identity) const;
    // Immediate natural-input entry: exact/continued familiarity, then Recall.
    // No disk, recording, shuffle or LLM precedes Recall. SHA-256 cue work is
    // part of Deja vu and is included in any input-to-Recall timing.
    [[nodiscard]] InputRecall input(std::string_view media, std::span<const std::byte> content) const;
    // Exact producer-declared scope under an authenticated input receipt.
    // A missing scope never falls back to unrelated dialogue/continuation.
    [[nodiscard]] InputRecall input_scope(const InputRecall&, std::string_view scope) const;
    // Follow sealed observation-context links from one already replayed
    // original. No scope name, log scan or payload read is needed to Recall.
    [[nodiscard]] InputRecall related(const ReplayedInput&) const;
    // Only receipt metadata is considered here; one original is read later.
    [[nodiscard]] std::optional<std::size_t> select_replay(const InputRecall&) const;
    [[nodiscard]] ReplayedInput replay(const InputRecall& recalled, std::size_t candidate) const;
    // Reissue a temporary Replay after restart from authenticated input and
    // ancestor head. Boundary comes from the owner, never supplied JSON counts.
    [[nodiscard]] ReplayedInput restore_temporary_replay(const ExperienceLocation& input,
        std::string_view scope,const architecture::DigestBytes& connection,const ExperienceLocation& remembered_head,
        std::size_t original_index,const ExperienceLocation& original) const;
    [[nodiscard]] ReplayedInput restore_main_replay(const ExperienceLocation& input,
        std::string_view scope,const architecture::DigestBytes& connection,const ExperienceLocation& remembered_head,
        const ExperienceLocation& observation_head,std::size_t original_index,const ExperienceLocation& original) const;
    [[nodiscard]] ReplayedInput restore_replay(const ExperienceLocation& input,const ReplayRecovery&) const;
    // Partial original access preserves Recall provenance but does not issue a
    // Replay receipt or update the continuation key.
    [[nodiscard]] EvidencePayloadSlice read_payload_slice(const InputRecall&, std::size_t candidate,
        std::uint64_t offset, std::uint64_t count) const;
    // Compare only sealed observations appended after Recall, without making
    // recalled experience new evidence or committing another strength update.
    [[nodiscard]] ReplayComparison compare_replay(const ReplayedInput&, std::uint64_t seed,
        std::uint64_t step) const;
    // A conflict receipt opens re-evidence; newly arrived observations are read
    // again. Agreement, abstention and receipts from other routes are rejected.
    [[nodiscard]] ReEvidenceResult re_evidence(const ReplayedInput&, const ReplayComparison&,
        std::uint64_t seed, std::uint64_t step) const;
    [[nodiscard]] StoredExperience replay(const RecallCandidates& candidates, std::size_t candidate,
        std::size_t original_index) const;
private:
    [[nodiscard]] ReEvidenceResult evaluate_replay(const ReplayedInput&, std::uint64_t seed,
        std::uint64_t step) const;
    [[nodiscard]] InputMatch selected_input(const InputRecall&, std::size_t candidate) const;
    SessionRuntime& temporary_;
    MemoryBudget& memory_;
    // A retained receipt keeps this allocation alive, so reusing a router's
    // object address cannot reuse its issuing identity.
    std::shared_ptr<const std::byte> issuer_;
    std::pmr::vector<const SessionRuntime*> mounted_;
    std::pmr::map<architecture::DigestBytes, std::pmr::vector<RecallMatch>> main_;
    struct MainCue { const SessionRuntime* session; CueReference reference; };
    const PersistentMainGraph* merged_main_ = nullptr;
    ExperienceLocation merged_head_;
    void require_main_current() const;
    [[nodiscard]] RecallMatch merged_match(const architecture::DigestBytes& identity) const;
    std::pmr::map<architecture::DigestBytes, std::pmr::vector<MainCue>> main_cues_;
    // The active session's recorded context may start a dialogue before any
    // Replay. Only actual sealed context-index entries can produce candidates.
    const architecture::DigestBytes session_context_;
    // Main-owned dialogue continuity, updated only after a successful selected
    // Replay. It holds an experience key, never copied dialogue text or a verdict.
    mutable std::optional<architecture::DigestBytes> continuation_;
    mutable std::optional<architecture::DigestBytes> continued_context_;
    [[nodiscard]] InputRecall recall_cue(const architecture::DigestBytes& cue,
        architecture::kernel::RecallScope scope, architecture::kernel::FamiliarityKey kind,
        const architecture::DigestBytes* context = nullptr, bool seed_only = false) const;
};

}  // namespace swegca::vrs
