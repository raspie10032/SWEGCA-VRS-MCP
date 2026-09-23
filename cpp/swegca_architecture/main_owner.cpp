#include "swegca_architecture/main_owner.hpp"

#include "swegca_architecture/authority.hpp"
#include "swegca_architecture/allocation.hpp"

#include <atomic>
#include <memory>
#include <stdexcept>
#include <utility>

namespace swegca::architecture {
namespace {

std::atomic<bool> main_lifetime_active{false};

// The user's CognitiveState source defines the tensor invariants. Selecting
// a bounded borrowed reader is a C++ startup/recovery extension.
// SWEGCA: src/tinylm_slicer/mosaic_cognitive_kernel.py@5901a5a:220-254
CognitiveTensor initial_tensor(const AllocationContext& account,
                               const MainInitialState::TensorInput& input) {
    if (input.reader != nullptr) {
        if (!input.canonical_bytes.empty())
            throw std::invalid_argument("main_initial_tensor_source_ambiguous");
        return CognitiveTensor(account, input.scalar_type, input.shape, *input.reader);
    }
    return CognitiveTensor(account, input.scalar_type, input.shape,
                           input.canonical_bytes);
}

// The lease lasts until the final Main snapshot releases it.
// Destroying Main while a snapshot survives cannot open a second Main.
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
// share the host-supplied allocation context. The VRS host counts and judges
// resources; Main retains its authority and single-owner lifetime.
struct detail::MainOwnerState final {
    // SWEGCA: user@2026-09-23:1
    MainOwnerState(std::shared_ptr<MainLifetime> lifetime, AllocationContext allocation,
                   std::unique_ptr<MainAuthorityLedger> authority,
                   std::shared_ptr<const CognitiveState> current)
        : lifetime(std::move(lifetime)), allocation(std::move(allocation)),
          authority(std::move(authority)), current(std::move(current)) {}
    std::shared_ptr<MainLifetime> lifetime;
    AllocationContext allocation;
    std::unique_ptr<MainAuthorityLedger> authority;
    std::shared_ptr<const CognitiveState> current;
};

// Only this non-inline member exercises Main's private construction rights.
// The non-member storage type only receives already constructed objects.
// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:222-230
MainOwner::MainOwner(MainInitialState initial, AllocationContext account) {
    auto lifetime = std::make_shared<MainLifetime>();
    auto authority = std::unique_ptr<MainAuthorityLedger>(new MainAuthorityLedger(account));
    RoleRegistry roles(account, initial.roles);
    EvidenceReferences evidence(account.allocator<ExperienceAddress>());
    evidence.reserve(initial.evidence_references.size());
    for (const auto address : initial.evidence_references)
        evidence.emplace_back(account, address);
    CognitiveTensor semantic = initial_tensor(account, initial.semantic);
    CognitiveTensor executive = initial_tensor(account, initial.executive);
    CognitiveTensor scratch = initial_tensor(account, initial.scratch);
    StructuredWorldGraph graph(account, initial.entities, initial.relations);
    GoalState goals(CanonicalPayload(account, initial.goals));
    ValueState values(CanonicalPayload(account, initial.values));
    SelfState self(CanonicalPayload(account, initial.self));
    auto current = std::allocate_shared<CognitiveState>(
        account.allocator<CognitiveState>(), InitialStateKey{},
        OwnerId(account, initial.owner), std::move(roles),
        std::move(semantic), std::move(executive), std::move(scratch),
        std::move(graph), std::move(evidence),
        std::move(goals), std::move(values), std::move(self));

    state_ = std::make_unique<detail::MainOwnerState>(
        std::move(lifetime), std::move(account), std::move(authority),
        std::move(current));
}

// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:222-230
MainOwner::~MainOwner() = default;

// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:222-230
StateSnapshot MainOwner::snapshot() const {
    return StateSnapshot(state_->current, state_->lifetime);
}

}  // namespace swegca::architecture
