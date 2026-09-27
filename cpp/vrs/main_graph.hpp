#pragma once
#include "vrs/session_runtime.hpp"
#include "vrs/connection_regions.hpp"

namespace swegca::vrs {

// Main's in-memory graph merge owner. Sources must be explicitly ended and
// published; their original stores must outlive graph reads. Source runtime
// caches may be released after merging. All externally visible mutations
// are serialized by Main. Durable merged-root publication is a separate gate.
class MainGraph final {
public:
    class PreparedMerge;
    MainGraph(MemoryBudget& memory, double initial_strength, const architecture::EvidencePolicy& policy, std::uint32_t workers = 1, std::size_t region_capacity = 256);
    MainGraph(const MainGraph&) = delete;
    MainGraph& operator=(const MainGraph&) = delete;
    // False for an already merged source or a source without connections.
    // A failed batch leaves all previous connections and generation intact.
    [[nodiscard]] bool merge(const SessionRuntime& source, std::uint64_t seed, std::uint64_t step);
    // Preparation reads the published graph and ended source without changing
    // either. Readers may run concurrently; no graph writer may run until all
    // preparation tasks finish. Source stores and memory outlive the batch.
    [[nodiscard]] PreparedMerge prepare_merge(const SessionRuntime&, std::uint64_t seed, std::uint64_t step) const;
    // Publication remains serialized by Main, after preparation has joined.
    [[nodiscard]] bool commit_merge(PreparedMerge&&);
    // Borrowed views remain valid until a subsequent successful merge replaces
    // that connection. Readers and merges are serialized by Main.
    [[nodiscard]] const Connection* find(const architecture::DigestBytes& identity) const noexcept;
    [[nodiscard]] const ConnectionRefinement* refinement(const architecture::DigestBytes& identity) const noexcept;
    [[nodiscard]] StoredExperience replay(const architecture::DigestBytes& identity, std::size_t index) const;
    [[nodiscard]] EvidencePayloadSlice read_payload_slice(const architecture::DigestBytes&, std::size_t index,
        std::uint64_t offset, std::uint64_t count) const;
    // Serialized cache placement; never changes the graph generation or strength.
    [[nodiscard]] bool page_out(const architecture::DigestBytes& connection,std::size_t index,
        const std::filesystem::path& path,const architecture::DigestBytes& page,StorageBudget* storage=nullptr) const;
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }
    [[nodiscard]] std::optional<ExperienceSequence::PagePreparation> prepare_page(const architecture::DigestBytes& key,std::size_t index) const {
        const auto* entry=connections_.find(key);
        if(!entry)throw std::out_of_range("Main page prepare connection");
        return entry->connection.prepare_page(index);
    }
    [[nodiscard]] std::optional<architecture::DigestBytes> next_connection(const architecture::DigestBytes& lower) const noexcept {
        return connections_.next_key(lower);
    }
    [[nodiscard]] bool page_candidate(const architecture::DigestBytes& key,std::size_t& index) const noexcept {
        const auto* entry=connections_.find(key);
        return entry&&entry->connection.page_candidate(index);
    }
    [[nodiscard]] bool has_source(const architecture::DigestBytes& identity) const noexcept { return merged_.contains(identity); }
    [[nodiscard]] std::size_t region_count() const noexcept { return connections_.region_count(); }
    [[nodiscard]] std::size_t largest_region() const noexcept { return connections_.largest_region(); }
    [[nodiscard]] std::size_t source_count() const noexcept { return merged_.size(); }
    struct PortalIndexStats {std::size_t cue_keys=0,context_keys=0,cue_ranges=0,context_ranges=0;};
    // Serialized diagnostic only: counts address topology, never evaluates
    // evidence or enters the input/Recall hot path. No payload I/O/allocation.
    [[nodiscard]] PortalIndexStats portal_index_stats() const noexcept;

private:
    friend class PersistentMainGraph;
    friend class ExperienceRouter;
    using MergeSink = void (*)(void*, const architecture::DigestBytes&, const ExperienceLocation&,
        const architecture::DigestBytes&, std::uint64_t, std::uint64_t, std::uint64_t);
    bool merge_impl(const SessionRuntime&, std::uint64_t, std::uint64_t, MergeSink, void*);
    bool commit_impl(PreparedMerge&, MergeSink, void*);
    // Consecutive originals from one source share a store and read bound.
    struct Origin { const SessionStore* store; std::uint64_t read_limit; std::size_t end; };
    [[nodiscard]] const Origin& original_source(const architecture::DigestBytes&, std::size_t index) const;
    struct Entry {
        Entry(const architecture::DigestBytes& identity, double strength,
            const architecture::kernel::EvidenceRules& rules, MemoryBudget& memory);
        Connection connection;
        std::pmr::vector<Origin> origins;
        std::optional<ConnectionRefinement> report;
    };
    MemoryBudget& memory_;
    double initial_strength_;
    std::uint32_t workers_;
    architecture::kernel::EvidenceRules rules_;
    architecture::DigestBytes policy_digest_;
    ConnectionRegions<Entry> connections_;
    // Recorded context links, not semantic scores. Ranges point into existing
    // connections; originals and observation cardinalities are never copied.
    using PortalReference=std::pair<architecture::DigestBytes,std::size_t>;
    using PortalRanges=std::pmr::map<PortalReference,std::size_t>;
    using ContextPortals=std::pmr::map<architecture::DigestBytes,PortalRanges>;
    ContextPortals contexts_;
    ContextPortals cues_;
    std::pmr::map<architecture::DigestBytes, ExperienceLocation> merged_;
    std::uint64_t generation_ = 0;
};

// Opaque, single-use result of real shuffled SWEGCA evaluation. Callers cannot
// install their own verdict or change a prepared connection. The originating
// MainGraph and its memory/source stores must outlive this object.
class MainGraph::PreparedMerge final {
public:
    PreparedMerge(const PreparedMerge&) = delete;
    PreparedMerge& operator=(const PreparedMerge&) = delete;
    PreparedMerge(PreparedMerge&&) noexcept;
    PreparedMerge& operator=(PreparedMerge&&) = delete;
private:
    friend class MainGraph;
    PreparedMerge(const MainGraph&, std::uint64_t seed, std::uint64_t step);
    const MainGraph* owner_;
    std::uint64_t generation_, seed_, step_;
    std::pmr::map<architecture::DigestBytes, Entry> pending_;
    std::pmr::map<architecture::DigestBytes, ExperienceLocation> marker_;
    architecture::DigestBytes result_{};
    std::optional<ConnectionRegions<Entry>::Prepared> regions_;
    ContextPortals contexts_;
    ContextPortals cues_;
};

}  // namespace swegca::vrs
