#include "swegca_architecture/main_owner.hpp"

#include "swegca_architecture/authority.hpp"
#include "swegca_architecture/memory_ledger.hpp"

#include <atomic>
#include <memory>
#include <stdexcept>
#include <utility>

namespace swegca::architecture {
namespace {

std::atomic<bool> main_lifetime_active{false};

// The lease lasts until the final Main account or snapshot releases it.
// Destroying Main while a snapshot survives cannot open a second budget.
// This is process-local; the persistent directory owner lock must separately
// exclude other processes during experience/state storage integration.
// A retained snapshot deliberately prevents constructing another Main until
// it is released. This is not a mechanism for discarding old snapshots.
// Rule: reconstruction board §2.1 and §10.1 (one Main and persistent state).
class MainLifetime final {
public:
    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:83-93
    MainLifetime() {
        if (main_lifetime_active.exchange(true))
            throw std::logic_error("main_owner_already_live");
    }
    MainLifetime(const MainLifetime&) = delete;
    MainLifetime& operator=(const MainLifetime&) = delete;
    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:83-93
    ~MainLifetime() { main_lifetime_active.store(false); }
};

}  // namespace

// Initialization/ownership only: guarded successor publication, experience,
// evidence and action roles are integrated in their later architecture steps.
// Tensor/graph-array/payload allocations and state/control-block requests
// share one account. Identity strings, role and evidence-reference containers,
// bootstrap allocations, allocator overhead, stacks and mappings
// still require integration; memory_requested() is not an RSS guarantee.
struct detail::MainOwnerState final {
    std::unique_ptr<MemoryLedger> memory;
    std::unique_ptr<MainAuthorityLedger> authority;
    std::shared_ptr<const CognitiveState> current;
};

// Only this non-inline member exercises Main's private construction rights.
// The non-member storage type only receives already constructed objects.
// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:222-230
MainOwner::MainOwner(MainInitialState initial, std::uint64_t memory_limit) {
    auto memory = std::unique_ptr<MemoryLedger>(
        new MemoryLedger(memory_limit, std::make_shared<MainLifetime>()));
    auto authority = std::unique_ptr<MainAuthorityLedger>(new MainAuthorityLedger());
    const auto account = memory->account();
    CognitiveTensor semantic(account, initial.semantic.scalar_type,
                             initial.semantic.shape, initial.semantic.canonical_bytes);
    CognitiveTensor executive(account, initial.executive.scalar_type,
                              initial.executive.shape, initial.executive.canonical_bytes);
    CognitiveTensor scratch(account, initial.scratch.scalar_type,
                            initial.scratch.shape, initial.scratch.canonical_bytes);
    StructuredWorldGraph graph(account, initial.entities, initial.relations);
    GoalState goals(CanonicalPayload(account, initial.goals));
    ValueState values(CanonicalPayload(account, initial.values));
    SelfState self(CanonicalPayload(account, initial.self));
    auto current = std::allocate_shared<CognitiveState>(
        account.allocator<CognitiveState>(), InitialStateKey{},
        std::move(initial.owner), std::move(initial.roles),
        std::move(semantic), std::move(executive), std::move(scratch),
        std::move(graph), std::move(initial.evidence_references),
        std::move(goals), std::move(values), std::move(self));

    // Allocate storage before transferring the fully constructed ownership.
    // The three moves below do not allocate and cannot throw.
    state_ = std::make_unique<detail::MainOwnerState>();
    state_->memory = std::move(memory);
    state_->authority = std::move(authority);
    state_->current = std::move(current);
}

// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:222-230
MainOwner::~MainOwner() = default;

// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:222-230
StateSnapshot MainOwner::snapshot() const { return StateSnapshot(state_->current); }

// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3f:638-640
std::uint64_t MainOwner::memory_requested() const noexcept { return state_->memory->used(); }

}  // namespace swegca::architecture
