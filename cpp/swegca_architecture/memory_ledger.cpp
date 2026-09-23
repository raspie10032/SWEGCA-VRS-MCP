#include "swegca_architecture/memory_ledger.hpp"

#include <stdexcept>
#include <utility>

namespace swegca::architecture {

// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3f:638-640
MemoryLedger::MemoryLedger(std::uint64_t limit, std::shared_ptr<const void> owner_lifetime)
    : state_(std::make_shared<State>()) {
    if (limit == 0)
        throw std::invalid_argument("memory_ledger_limit_invalid");
    if (!owner_lifetime)
        throw std::invalid_argument("memory_ledger_owner_lifetime_missing");
    state_->owner_lifetime = std::move(owner_lifetime);
    state_->limit = limit;
}

// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3f:638-640
void MemoryLedger::charge(State& state, std::uint64_t bytes) {
    auto used = state.used.load();
    do {
        if (bytes > state.limit - used) throw std::runtime_error("memory_budget_exhausted");
    } while (!state.used.compare_exchange_weak(used, used + bytes));
}

// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3f:638-640
void MemoryLedger::discharge(State& state, std::uint64_t bytes) noexcept {
    state.used.fetch_sub(bytes);
}

// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3f:638-640
MemoryLedger::Hold MemoryLedger::Account::reserve(std::uint64_t bytes) const {
    charge(*state_, bytes);
    return Hold(state_, bytes);
}

// The carved state keeps the parent's charge (and Main's lifetime) until the
// last Account, Hold or Allocator on it is gone. Both objects are charged to
// the parent.
// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3f:638-640
MemoryLedger::Account MemoryLedger::Account::carve(std::uint64_t bytes) const {
    struct Carved {
        Hold charge;
        std::shared_ptr<const void> owner_lifetime;
    };
    auto charge = reserve(bytes);
    auto carved = std::allocate_shared<Carved>(Allocator<Carved>(state_),
                                               Carved{std::move(charge), state_->owner_lifetime});
    auto state = std::allocate_shared<State>(Allocator<State>(state_));
    state->owner_lifetime = std::move(carved);
    state->limit = bytes;
    return Account(std::move(state));
}

// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3f:638-640
MemoryLedger::Hold::Hold(Hold&& other) noexcept
    : state_(std::move(other.state_)), bytes_(std::exchange(other.bytes_, 0)) {}

// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3f:638-640
MemoryLedger::Hold::~Hold() {
    if (state_ && bytes_ != 0) discharge(*state_, bytes_);
}

// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3f:638-640
void MemoryLedger::Hold::merge(Hold&& other) {
    if (this == &other || other.bytes_ == 0) return;
    if (state_ && bytes_ != 0 && state_ != other.state_)
        throw std::logic_error("memory_hold_ledger_mismatch");
    state_ = std::move(other.state_);
    bytes_ += std::exchange(other.bytes_, 0);
}

// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3f:638-640
MemoryLedger::Hold MemoryLedger::Hold::split(std::uint64_t bytes) {
    if (bytes > bytes_) throw std::logic_error("memory_hold_split_invalid");
    bytes_ -= bytes;
    return Hold(state_, bytes);
}

}  // namespace swegca::architecture
