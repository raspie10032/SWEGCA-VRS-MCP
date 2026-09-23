#pragma once

#include "swegca_architecture/authority_roles.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <utility>

// Main's resident-memory ledger. What this type guarantees: only MainOwner
// can construct one (with the nonzero limit its host gives it), it cannot
// be copied or moved, and a
// component can only charge it through an Account it was given. What it does
// not guarantee by itself: that the process has one ledger. That is a
// premise of Main integration (MainOwner owns exactly one and hands the same
// Account to every component), closed there, not here.
//
// What a charge counts: the bytes a component asks for, before it asks. A
// Hold covers memory the component sizes itself (reserve before building);
// an Allocator charges each allocation of the container or shared object it
// is given to with its exact requested size (nodes, arrays, control blocks).
// It does not reach allocations those elements make themselves (for example
// a std::string inside a map value), which the component must charge with a
// Hold, nor heap headers and rounding, stacks or mappings. The ledger is
// exact for requested bytes; it is not by itself the 4 GB resident bound,
// which Main integration closes over every allocation.
//
// Lifetime contract: the counter lives as long as any Account, Hold or
// Allocator on it, so a charge released after MainOwner dropped its ledger
// still returns to the same counter. A Hold must outlive the memory it
// covers: declare it before that memory (members are destroyed in reverse
// order), and never assign over a Hold, which would release its charge
// while the memory still exists (move assignment is deleted for that reason).
namespace swegca::architecture {

class MemoryLedger final {
    struct State {
        // Keeps Main's exclusive process lifetime while accounts/snapshots live.
        std::shared_ptr<const void> owner_lifetime;
        std::atomic<std::uint64_t> used{0};
        std::uint64_t limit = 0;
    };

public:
    class Hold;
    class Account;
    template <class T>
    class Allocator;

    // A charge of a fixed number of bytes, returned when the Hold is destroyed.
    class Hold final {
    public:
        // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3f:638-640
        Hold() noexcept = default;
        Hold(Hold&& other) noexcept;
        Hold& operator=(Hold&&) = delete;
        Hold(const Hold&) = delete;
        Hold& operator=(const Hold&) = delete;
        ~Hold();

        // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3f:638-640
        [[nodiscard]] std::uint64_t bytes() const noexcept { return bytes_; }
        // Takes over `other`'s charge. Both must be on the same ledger (or
        // one of them empty); otherwise `memory_hold_ledger_mismatch`.
        void merge(Hold&& other);
        // Moves `bytes` of this charge into a new Hold, for memory that
        // changes owner; more than this Hold carries is
        // `memory_hold_split_invalid`.
        [[nodiscard]] Hold split(std::uint64_t bytes);

    private:
        friend class MemoryLedger;
        friend class Account;
        // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3f:638-640
        Hold(std::shared_ptr<State> state, std::uint64_t bytes) noexcept
            : state_(std::move(state)), bytes_(bytes) {}

        std::shared_ptr<State> state_;
        std::uint64_t bytes_ = 0;
    };

    // What a Main-owned component keeps: it can charge the ledger, never
    // change its limit or make another ledger.
    class Account final {
    public:
        // Fails with `memory_budget_exhausted` when `bytes` do not fit.
        [[nodiscard]] Hold reserve(std::uint64_t bytes) const;
        // A budget carved out of this ledger: `bytes` are charged here now
        // (`memory_budget_exhausted` when they do not fit) and stay charged
        // while any Account, Hold or Allocator on the carved budget lives.
        // Charges on the carved budget count against `bytes` only, so a
        // component given one can never take more of this ledger, and no
        // other component can take the part it was given. The carved state
        // and its bookkeeping (a few hundred bytes) are charged here too.
        [[nodiscard]] Account carve(std::uint64_t bytes) const;
        // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3f:638-640
        [[nodiscard]] std::uint64_t used() const noexcept { return state_->used.load(); }
        // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3f:638-640
        template <class T>
        [[nodiscard]] Allocator<T> allocator() const noexcept {
            return Allocator<T>(state_);
        }

    private:
        friend class MemoryLedger;
        // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3f:638-640
        explicit Account(std::shared_ptr<State> state) noexcept : state_(std::move(state)) {}

        std::shared_ptr<State> state_;
    };

    // Standard allocator that charges each allocation's exact requested size
    // before allocating and returns it after deallocating.
    // Not final: standard containers derive from their allocator.
    template <class T>
    class Allocator {
    public:
        using value_type = T;

        // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3f:638-640
        template <class U>
        Allocator(const Allocator<U>& other) noexcept : state_(other.state_) {}

        // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3f:638-640
        [[nodiscard]] T* allocate(std::size_t count) {
            if (count > std::numeric_limits<std::size_t>::max() / sizeof(T))
                throw std::bad_array_new_length();
            const auto bytes = static_cast<std::uint64_t>(count * sizeof(T));
            charge(*state_, bytes);
            try {
                return std::allocator<T>{}.allocate(count);
            } catch (...) {
                discharge(*state_, bytes);
                throw;
            }
        }

        // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3f:638-640
        void deallocate(T* pointer, std::size_t count) noexcept {
            std::allocator<T>{}.deallocate(pointer, count);
            discharge(*state_, static_cast<std::uint64_t>(count * sizeof(T)));
        }

        // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3f:638-640
        template <class U>
        [[nodiscard]] bool operator==(const Allocator<U>& other) const noexcept {
            return state_ == other.state_;
        }

    private:
        template <class>
        friend class Allocator;
        friend class Account;
        // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3f:638-640
        explicit Allocator(std::shared_ptr<State> state) noexcept : state_(std::move(state)) {}

        std::shared_ptr<State> state_;
    };

    MemoryLedger(const MemoryLedger&) = delete;
    MemoryLedger& operator=(const MemoryLedger&) = delete;
    MemoryLedger(MemoryLedger&&) = delete;
    MemoryLedger& operator=(MemoryLedger&&) = delete;
    ~MemoryLedger() = default;

    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3f:638-640
    [[nodiscard]] Account account() const noexcept { return Account(state_); }
    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3f:638-640
    [[nodiscard]] std::uint64_t used() const noexcept { return state_->used.load(); }
    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3f:638-640
    [[nodiscard]] std::uint64_t limit() const noexcept { return state_->limit; }

private:
    friend class MainOwner;
    // `limit` is the host's budget and must be nonzero
    // (`memory_ledger_limit_invalid`).
    MemoryLedger(std::uint64_t limit, std::shared_ptr<const void> owner_lifetime);

    // Cap before add: `used` never passes `limit`, even transiently.
    static void charge(State& state, std::uint64_t bytes);
    static void discharge(State& state, std::uint64_t bytes) noexcept;

    std::shared_ptr<State> state_;
};

}  // namespace swegca::architecture
