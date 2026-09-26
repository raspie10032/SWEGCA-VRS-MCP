#pragma once
#include "vrs/session_runtime.hpp"

namespace swegca::vrs {

// Main's in-memory graph merge owner. Sources must be explicitly ended and
// published; their original stores must outlive graph reads. Source runtime
// caches may be released after merging. All externally visible mutations
// are serialized by Main. Durable merged-root publication is a separate gate.
class MainGraph final {
public:
    MainGraph(MemoryBudget& memory, double initial_strength, const architecture::EvidencePolicy& policy, std::uint32_t workers = 1);
    MainGraph(const MainGraph&) = delete;
    MainGraph& operator=(const MainGraph&) = delete;
    // False for an already merged source or a source without connections.
    // A failed batch leaves all previous connections and generation intact.
    [[nodiscard]] bool merge(const SessionRuntime& source, std::uint64_t seed, std::uint64_t step);
    // Borrowed views remain valid until a subsequent successful merge replaces
    // that connection. Readers and merges are serialized by Main.
    [[nodiscard]] const Connection* find(const architecture::DigestBytes& identity) const noexcept;
    [[nodiscard]] const ConnectionRefinement* refinement(const architecture::DigestBytes& identity) const noexcept;
    [[nodiscard]] StoredExperience replay(const architecture::DigestBytes& identity, std::size_t index) const;
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }
    [[nodiscard]] bool has_source(const architecture::DigestBytes& identity) const noexcept { return merged_.contains(identity); }
    [[nodiscard]] std::size_t source_count() const noexcept { return merged_.size(); }
private:
    friend class PersistentMainGraph;
    friend class ExperienceRouter;
    using MergeSink = void (*)(void*, const architecture::DigestBytes&, const ExperienceLocation&,
        const architecture::DigestBytes&, std::uint64_t, std::uint64_t, std::uint64_t);
    bool merge_impl(const SessionRuntime&, std::uint64_t, std::uint64_t, MergeSink, void*);
    struct Origin { const SessionStore* store; std::uint64_t read_limit; };
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
    std::pmr::map<architecture::DigestBytes, Entry> connections_;
    std::pmr::map<architecture::DigestBytes, ExperienceLocation> merged_;
    std::uint64_t generation_ = 0;
};

}  // namespace swegca::vrs
