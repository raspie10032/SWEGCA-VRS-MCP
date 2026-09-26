#pragma once
#include "vrs/main_graph.hpp"

namespace swegca::vrs {

// Main's storage registry resolves the exact ended source named by a record.
// Its original stores outlive this graph. Recovery may release runtime caches.
class MainSourceResolver {
public:
    virtual ~MainSourceResolver() = default;
    virtual const SessionRuntime& resolve(const architecture::DigestBytes& identity) = 0;
};

class PersistentMainGraph final {
public:
    static PersistentMainGraph create(const std::filesystem::path&, const architecture::DigestBytes&,
        MemoryBudget&, double initial_strength, const architecture::EvidencePolicy&, std::uint64_t block_capacity, std::uint32_t workers = 1, StorageBudget* storage = nullptr);
    static PersistentMainGraph open(const std::filesystem::path&, const architecture::DigestBytes&,
        MemoryBudget&, double initial_strength, const architecture::EvidencePolicy&, MainSourceResolver&, std::uint32_t workers = 1, StorageBudget* storage = nullptr);
    PersistentMainGraph(const PersistentMainGraph&) = delete;
    PersistentMainGraph& operator=(const PersistentMainGraph&) = delete;
    PersistentMainGraph(PersistentMainGraph&&) = delete;
    PersistentMainGraph& operator=(PersistentMainGraph&&) = delete;
    [[nodiscard]] bool merge(const SessionRuntime&, std::uint64_t seed, std::uint64_t step);
    [[nodiscard]] const MainGraph& graph() const;
    [[nodiscard]] bool usable() const noexcept { return usable_; }
    [[nodiscard]] const ExperienceLocation& head() const noexcept { return head_; }
private:
    struct Record {
        std::uint64_t generation, seed, step;
        architecture::DigestBytes source, result;
        ExperienceLocation source_root, parent;
    };
    PersistentMainGraph(const std::filesystem::path&, const architecture::DigestBytes&, MemoryBudget&,
        double, const architecture::EvidencePolicy&, std::uint64_t, MainSourceResolver*, std::uint32_t, StorageBudget*);
    void next_block();
    void restore(MainSourceResolver&);
    static void persist(void*, const architecture::DigestBytes&, const ExperienceLocation&,
        const architecture::DigestBytes&, std::uint64_t, std::uint64_t, std::uint64_t);
    static void verify(void*, const architecture::DigestBytes&, const ExperienceLocation&,
        const architecture::DigestBytes&, std::uint64_t, std::uint64_t, std::uint64_t);
    std::filesystem::path directory_;
    architecture::DigestBytes identity_;
    MemoryBudget& memory_;
    StorageBudget* storage_;
    MainGraph graph_;
    std::optional<ExperienceBlock> control_, writer_;
    std::uint64_t capacity_ = 0, next_index_ = 0;
    ExperienceLocation head_;
    bool usable_ = true;
};

}  // namespace swegca::vrs
