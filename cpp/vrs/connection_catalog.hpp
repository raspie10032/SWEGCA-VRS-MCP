#pragma once

#include "vrs/persistent_connection.hpp"

#include <map>
#include <stdexcept>

namespace swegca::vrs {

struct HeadUpdate {
    const PersistentConnection* connection = nullptr;
    ExperienceLocation expected;
};

// Main-owned latest-head table for one temporary session. On-disk roots are
// immutable deltas; only their fixed-size current pointer is replaced. The
// ended session binds this pointer and can publish it with its originals.
// Global Main/temporary lookup and cross-session graph merge are still separate.
class ConnectionCatalog final {
public:
    ConnectionCatalog(SessionStore& session, MemoryBudget& memory, std::uint64_t original_read_limit);
    ~ConnectionCatalog();
    ConnectionCatalog(const ConnectionCatalog&) = delete;
    ConnectionCatalog& operator=(const ConnectionCatalog&) = delete;

    // The first empty root is allowed once an original record exists. No root
    // is created by opening the catalog. All changes in a batch publish together.
    void publish(std::span<const HeadUpdate> changes);
    [[nodiscard]] const architecture::kernel::ConnectionHead* find(const architecture::DigestBytes& identity) const;
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }
    [[nodiscard]] const ExperienceLocation& root() const noexcept { return root_; }
    [[nodiscard]] bool usable() const noexcept { return usable_; }
    [[nodiscard]] std::size_t size() const noexcept { return heads_.size(); }
    [[nodiscard]] PersistentConnection recover(const architecture::DigestBytes& identity) const;
    [[nodiscard]] const auto& heads() const {
        if (!usable_ || !session_.usable()) throw std::logic_error("catalog unavailable");
        return heads_;
    }

private:
    void restore();
    SessionStore& session_;
    MemoryBudget& memory_;
    std::uint64_t original_read_limit_;
    std::filesystem::path directory_;
    int lock_ = -1;
    bool usable_ = true;
    std::uint64_t generation_ = 0;
    ExperienceLocation root_;
    std::pmr::map<architecture::DigestBytes, architecture::kernel::ConnectionHead> heads_;
};

}  // namespace swegca::vrs
