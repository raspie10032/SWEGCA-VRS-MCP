#pragma once
#include "vrs/persistent_main_graph.hpp"

namespace swegca::vrs {

// Main owns this registry before constructing its graph. It retains original
// stores for graph Replay; decoded session caches are needed only for merging.
// Discovery never ends or publishes an active session.
class MainSources final : public MainSourceResolver {
public:
    MainSources(const std::filesystem::path& root, MemoryBudget&, std::uint64_t read_limit, StorageBudget* storage = nullptr);
    MainSources(const MainSources&) = delete;
    MainSources& operator=(const MainSources&) = delete;
    [[nodiscard]] std::pmr::vector<architecture::DigestBytes> published() const;
    const SessionRuntime& resolve(const architecture::DigestBytes&) override;
    void release_caches() noexcept;
    // Work pump for the Main owner, outside input/Recall. Each source has its
    // own durable commit; a later failure never rolls back earlier commits.
    [[nodiscard]] std::size_t merge_published(PersistentMainGraph&, std::uint64_t seed, std::uint64_t step);
private:
    friend class Runtime;
    SessionRuntime& acquire_session(const architecture::DigestBytes&, std::string_view,
        std::uint64_t block_capacity, bool resume);
    void release_session(const architecture::DigestBytes&, bool keep_cache) noexcept;
    struct Source {
        Source(const std::filesystem::path&, const architecture::DigestBytes&, MemoryBudget&, std::uint64_t, StorageBudget*, bool active = false,
            bool resume = true, std::string_view name = {}, std::uint64_t block_capacity = 0);
        ~Source();
        Source(const Source&) = delete;
        Source& operator=(const Source&) = delete;
        SessionRuntime& runtime();
        void release_cache() noexcept;
        MemoryBudget& memory;
        std::uint64_t read_limit;
        SessionStore* store = nullptr;
        SessionRuntime* cache = nullptr;
        bool leased = false;
    };
    std::filesystem::path root_;
    MemoryBudget& memory_;
    StorageBudget* storage_;
    std::uint64_t read_limit_;
    std::pmr::map<architecture::DigestBytes, Source> sources_;
};
} // namespace swegca::vrs
