#pragma once
#include "vrs/main_sources.hpp"
#include "vrs/storage_inventory.hpp"
#include <thread>
#include <exception>

namespace swegca::vrs {
struct ReceivedInput {
    InputRecall recalled;
    RecordedRefinement recorded;
};

// Main-owned composition of the selected Replay and its core comparison.
// Selection and this receipt confer no truth or external-action authority.
struct InputCognition {
    std::size_t candidate;
    ReplayedInput replayed;
    ReplayComparison comparison;
    std::optional<ReEvidenceResult> reverified;
    [[nodiscard]] const ReEvidenceResult& assessment() const noexcept {
        return reverified ? *reverified : comparison.evidence();
    }
};

struct RuntimeConfig {
    architecture::DigestBytes main_identity;
    architecture::EvidencePolicy policy;
    double initial_strength;
    std::uint64_t session_block_capacity, main_block_capacity, read_limit;
    std::uint32_t merge_workers = 1;
    std::uint64_t storage_bytes = 500000000000ULL;
    std::uint64_t io_bytes_per_second = 625000000;
    std::size_t memory_target_bytes = 0; // zero selects 75% of the caller's RAM budget
};

// Main's serialized lifecycle owner. Destruction never implies session end.
// Public calls remain serialized on the owner. An internal preparation worker
// may run alongside them; only poll_work publishes completed Main work.
// Borrowed Recall/Replay receipts expire when the session ends.
class Runtime final {
public:
    static Runtime create(const std::filesystem::path&, const RuntimeConfig&, MemoryBudget&);
    static Runtime ensure(const std::filesystem::path&, const RuntimeConfig&, MemoryBudget&);
    static Runtime open(const std::filesystem::path&, const RuntimeConfig&, MemoryBudget&);
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    Runtime(Runtime&&) = delete;
    Runtime& operator=(Runtime&&) = delete;
    ~Runtime();
    void start_session(const architecture::DigestBytes&, std::string_view name);
    void resume_session(const architecture::DigestBytes&);
    // Attach additional live routes to this same Main. Attachment does not
    // select, end, publish or merge a session. Public calls remain serialized.
    void attach_session(const architecture::DigestBytes&, std::string_view name);
    void attach_resumed_session(const architecture::DigestBytes&);
    // Lifecycle setup only: filesystem discovery must not precede each input.
    void attach_available_session(const architecture::DigestBytes&,std::string_view name);
    void select_session(const architecture::DigestBytes&);
    [[nodiscard]] std::size_t attached_sessions() const noexcept { return sessions_.size(); }
    void end_session();
    // Explicitly close one attached session without changing another selection.
    void end_session(const architecture::DigestBytes&);
    [[nodiscard]] const StorageBudget& storage() const noexcept { return storage_; }
    [[nodiscard]] bool has_session() const noexcept { return active_!=nullptr; }
    [[nodiscard]] const SessionRuntime& session() const;
    [[nodiscard]] const SessionRuntime& attached_session(const architecture::DigestBytes&) const;
    [[nodiscard]] const PersistentMainGraph& main() const noexcept { return main_; }
    // Recall before recording this event, then synchronously retain its full
    // original through SWEGCA. No choice of Replay candidate is invented here.
    [[nodiscard]] ReceivedInput receive(const OriginalExperienceView&, std::uint64_t seed, std::uint64_t step);
    // Natural cue and exact native envelope bind to one original record. Recall
    // precedes recording; the cue is persisted under the same record checksum.
    [[nodiscard]] ReceivedInput receive_envelope(std::string_view cue_media,
        std::span<const std::byte> cue_content, const OriginalExperienceView& envelope,
        std::uint64_t seed, std::uint64_t step);
    [[nodiscard]] InputRecall input(std::string_view media, std::span<const std::byte> content) const;
    [[nodiscard]] std::optional<InputCognition> cognize(const InputRecall&, std::uint64_t seed,
        std::uint64_t step) const;
    [[nodiscard]] std::optional<std::size_t> select_replay(const InputRecall&) const;
    [[nodiscard]] ReplayedInput replay(const InputRecall&, std::size_t candidate) const;
    [[nodiscard]] EvidencePayloadSlice read_payload_slice(const InputRecall&, std::size_t candidate,
        std::uint64_t offset, std::uint64_t count) const;
    [[nodiscard]] ReplayComparison compare_replay(const ReplayedInput&, std::uint64_t seed, std::uint64_t step) const;
    [[nodiscard]] ReEvidenceResult re_evidence(const ReplayedInput&, const ReplayComparison&,
        std::uint64_t seed, std::uint64_t step) const;
    void save_cognition(const ExperienceLocation&, std::span<const std::byte> metadata);
    [[nodiscard]] architecture::DigestBytes save_cognition_revision(const ExperienceLocation&,
        std::span<const std::byte> metadata);
    [[nodiscard]] StoredExperience read_cognition_original(const architecture::DigestBytes& source,
        const ExperienceLocation&) const;
    // Explicit owner maintenance outside input and while no merge job exists.
    // Original stores remain authoritative; this only releases sealed metadata.
    [[nodiscard]] bool page_out_main(const architecture::DigestBytes&,std::size_t original_index);
    // Idle owner maintenance: visits at most one segment, never called by input.
    // true requests another idle opportunity; false means relieved/exhausted/busy.
    [[nodiscard]] bool maintain_memory();
    void define_connection(const architecture::DigestBytes&);
    [[nodiscard]] RecordedRefinement observe(const architecture::DigestBytes&, const OriginalExperienceView&,
        const architecture::kernel::EvidenceObservation&, std::uint64_t seed, std::uint64_t step);
    [[nodiscard]] RecordedRefinement retain(const OriginalExperienceView&, std::uint64_t seed, std::uint64_t step);
    // Drain only published ended sources and refresh the current query index.
    // Never called from input(). Failures leave durable work available to retry.
    [[nodiscard]] std::size_t work(std::uint64_t seed, std::uint64_t step);
    // Explicit background work scheduling, never called from input or end.
    // Returns false when no published unmerged source is available. While a
    // batch exists, later explicit ends join its queue using the same seed/step.
    [[nodiscard]] bool work_scheduled() const noexcept { return work_.has_value(); }
    [[nodiscard]] bool schedule_work(std::uint64_t seed, std::uint64_t step);
    // nullopt: preparation still running (or next source launched).
    // A value: this scheduled batch finished, with that many committed sources.
    // Readiness never waits for a running worker; ready publication may do I/O.
    [[nodiscard]] std::optional<std::size_t> poll_work();
private:
    struct Active {
        Active(SessionRuntime&, const architecture::DigestBytes&, MemoryBudget&, const PersistentMainGraph&);
        architecture::DigestBytes identity;
        SessionRuntime& runtime;
        ExperienceRouter router;
        ExperienceLocation indexed_main;
    };
    Runtime(const std::filesystem::path&, const RuntimeConfig&, MemoryBudget&, bool create, bool discover=false);
    [[nodiscard]] Active& require_session();
    [[nodiscard]] const Active& require_session() const;
    void require_active() const;
    void attach(const architecture::DigestBytes&, std::string_view, bool resume);
    void refresh_main();
    bool launch_next();
    [[nodiscard]] architecture::DigestBytes page_identity(
        const architecture::DigestBytes&,std::size_t);
    void discard_work() noexcept;
    struct Work {
        Work(std::pmr::vector<architecture::DigestBytes>&& ids, std::uint64_t seed, std::uint64_t step)
            : ids(std::move(ids)), seed(seed), step(step) {}
        // Owner-only queue; background preparation never accesses this vector.
        std::pmr::vector<architecture::DigestBytes> ids;
        std::uint64_t seed, step;
        std::size_t index=0, merged=0;
        MainSources::Source* source=nullptr;
        std::optional<MainGraph::PreparedMerge> prepared;
        std::exception_ptr failure;
        std::atomic<bool> done{false};
        // Must join before prepared/failure storage is destroyed.
        std::jthread thread;
    };
    std::filesystem::path root_;
    RuntimeConfig config_;
    MemoryBudget& memory_;
    StorageRoot storage_root_;
    StorageBudget storage_;
    MainSources sources_;
    PersistentMainGraph main_;
    std::pmr::map<architecture::DigestBytes,Active> sessions_;
    Active* active_=nullptr;
    std::optional<Work> work_;
    std::uint64_t page_attempt_ = 0;
    architecture::DigestBytes page_cursor_{};
    std::size_t page_index_=0;
    struct PageWork {
        explicit PageWork(ExperienceSequence::PagePreparation&& value):prepared(std::move(value)){}
        ExperienceSequence::PagePreparation prepared;
        std::exception_ptr failure;
        std::atomic<bool> done{false};
        std::jthread thread; // joins before prepared and its segment pin are destroyed
    };
    std::optional<PageWork> page_work_;
};
} // namespace swegca::vrs
