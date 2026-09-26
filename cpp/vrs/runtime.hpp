#pragma once
#include "vrs/main_sources.hpp"

namespace swegca::vrs {
struct RuntimeConfig {
    architecture::DigestBytes main_identity;
    architecture::EvidencePolicy policy;
    double initial_strength;
    std::uint64_t session_block_capacity, main_block_capacity, read_limit;
};

// Main's serialized lifecycle owner. Destruction never implies session end.
// Input and background work are separate entries; callers do not run them
// concurrently. Borrowed Recall/Replay receipts expire when the session ends.
class Runtime final {
public:
    static Runtime create(const std::filesystem::path&, const RuntimeConfig&, MemoryBudget&);
    static Runtime open(const std::filesystem::path&, const RuntimeConfig&, MemoryBudget&);
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    Runtime(Runtime&&) = delete;
    Runtime& operator=(Runtime&&) = delete;
    void start_session(const architecture::DigestBytes&, std::string_view name);
    void resume_session(const architecture::DigestBytes&);
    void end_session();
    [[nodiscard]] bool has_session() const noexcept { return active_.has_value(); }
    [[nodiscard]] const SessionRuntime& session() const;
    [[nodiscard]] const PersistentMainGraph& main() const noexcept { return main_; }
    [[nodiscard]] InputRecall input(std::string_view media, std::span<const std::byte> content) const;
    [[nodiscard]] ReplayedInput replay(const InputRecall&, std::size_t candidate) const;
    [[nodiscard]] ReEvidenceResult re_evidence(const ReplayedInput&, std::uint64_t seed, std::uint64_t step) const;
    void define_connection(const architecture::DigestBytes&);
    [[nodiscard]] RecordedRefinement observe(const architecture::DigestBytes&, const OriginalExperienceView&,
        const architecture::kernel::EvidenceObservation&, std::uint64_t seed, std::uint64_t step);
    [[nodiscard]] RecordedRefinement retain(const OriginalExperienceView&, std::uint64_t seed, std::uint64_t step);
    // Drain only published ended sources and refresh the current query index.
    // Never called from input(). Failures leave durable work available to retry.
    [[nodiscard]] std::size_t work(std::uint64_t seed, std::uint64_t step);
private:
    struct Active {
        Active(const std::filesystem::path&, const architecture::DigestBytes&, std::string_view,
            const RuntimeConfig&, MemoryBudget&, const PersistentMainGraph&, bool resume);
        SessionStore store;
        SessionRuntime runtime;
        ExperienceRouter router;
        ExperienceLocation indexed_main;
    };
    Runtime(const std::filesystem::path&, const RuntimeConfig&, MemoryBudget&, bool create);
    [[nodiscard]] Active& require_session();
    [[nodiscard]] const Active& require_session() const;
    void require_active() const;
    std::filesystem::path root_;
    RuntimeConfig config_;
    MemoryBudget& memory_;
    MainSources sources_;
    PersistentMainGraph main_;
    std::optional<Active> active_;
};
} // namespace swegca::vrs
