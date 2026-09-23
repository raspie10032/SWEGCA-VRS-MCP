#include "swegca_vrs/main_owner.hpp"

#include "swegca_vrs/authority.hpp"
#include "swegca_vrs/allocation.hpp"
#include "swegca_vrs/main_commit_marker.hpp"
#include "swegca_vrs/state_recovery.hpp"

#include <atomic>
#include <memory>
#include <new>
#include <stdexcept>
#include <utility>

namespace swegca::vrs {
namespace {

std::atomic<bool> main_lifetime_active{false};

// The user's CognitiveState source defines the tensor invariants. Selecting
// a bounded borrowed reader is a C++ startup/recovery extension.
// SWEGCA: src/swegca/mosaic_cognitive_kernel.py@5901a5a:220-254
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
// This process-local guard is the C++ implementation of the source's one
// authoritative Main rule; the source does not prescribe this atomic flag.
class MainLifetime final {
public:
    // SWEGCA: paper/swegca/ARCHITECTURE_SPEC.md@5901a5a:103-107
    MainLifetime() {
        if (main_lifetime_active.exchange(true))
            throw std::logic_error("main_owner_already_live");
    }
    MainLifetime(const MainLifetime&) = delete;
    MainLifetime& operator=(const MainLifetime&) = delete;
    // SWEGCA: paper/swegca/ARCHITECTURE_SPEC.md@5901a5a:103-107
    ~MainLifetime() { main_lifetime_active.store(false); }
};

}  // namespace

// Initialization/ownership only: guarded successor publication, experience,
// evidence and action roles are integrated in their later architecture steps.
// Tensor/graph-array/payload allocations and the Main lifetime, authority,
// state holder and their control blocks share the host-supplied allocation
// context. The VRS host counts and judges resources; Main retains its
// authority and single-owner lifetime.
struct MainPublishedPair final {
    // SWEGCA: paper/swegca/ARCHITECTURE_SPEC.md@5901a5a:196-205
    MainPublishedPair(std::shared_ptr<const CognitiveState> current, PublishedStateId publication)
        : state(std::move(current)), head(std::move(publication)) {}
    std::shared_ptr<const CognitiveState> state;
    PublishedStateId head;
};

// A recovered record graph is still journal data. Its position and digest
// cannot become Main publication authority until marker selection and the
// strength root have been checked by Main.
struct MainRecoveredGenesis final {
    std::shared_ptr<const CognitiveState> state;
    DigestBytes content_digest;
    journal::RecordPosition publication;
};

struct detail::MainOwnerState final {
    // SWEGCA: paper/swegca/ARCHITECTURE_SPEC.md@5901a5a:17-27
    MainOwnerState(std::shared_ptr<MainLifetime> lifetime, AllocationContext allocation,
                   std::shared_ptr<MainAuthorityLedger> authority,
                   std::shared_ptr<const CognitiveState> current)
        : lifetime(std::move(lifetime)), allocation(std::move(allocation)),
          authority(std::move(authority)), initial(std::move(current)) {}
    std::shared_ptr<MainLifetime> lifetime;
    AllocationContext allocation;
    std::shared_ptr<MainAuthorityLedger> authority;
    // Before genesis there is only initial content. Once Main has verified
    // and selected a committed publication, the state and its exact head
    // enter one immutable object. A reader can never observe a state from
    // one publication with the head of another, including a bit-exact
    // rollback whose content digest equals an earlier publication.
    std::shared_ptr<const CognitiveState> initial;
    std::atomic<std::shared_ptr<const MainPublishedPair>> published;
};

// Only Main exercises the private initial-state construction key. The same
// validation is used for fresh genesis and an exact recovered genesis stream.
// SWEGCA: paper/swegca/ARCHITECTURE_SPEC.md@5901a5a:103-107
std::shared_ptr<const CognitiveState> MainOwner::make_initial_state(
    MainInitialState initial, const AllocationContext& account) {
    RoleRegistry roles(account, initial.roles);
    EvidenceReferences evidence(account.allocator<ExperienceAddress>());
    evidence.reserve(initial.evidence_references.size());
    // The source CognitiveState accepts state-level reference strings without
    // validation. Native ExperienceAddress narrows that set (blank/NUL/UTF-8/
    // byte limit); keep order and repeats, and do not claim Python parity.
    for (const auto address : initial.evidence_references)
        evidence.emplace_back(account, address);
    CognitiveTensor semantic = initial_tensor(account, initial.semantic);
    CognitiveTensor executive = initial_tensor(account, initial.executive);
    CognitiveTensor scratch = initial_tensor(account, initial.scratch);
    StructuredWorldGraph graph(account, initial.entities, initial.relations);
    GoalState goals(CanonicalPayload(account, initial.goals));
    ValueState values(CanonicalPayload(account, initial.values));
    SelfState self(CanonicalPayload(account, initial.self));
    return std::allocate_shared<CognitiveState>(
        account.allocator<CognitiveState>(), InitialStateKey{},
        OwnerId(account, initial.owner), std::move(roles),
        std::move(semantic), std::move(executive), std::move(scratch),
        std::move(graph), std::move(evidence),
        std::move(goals), std::move(values), std::move(self));
}

// A checked genesis record graph can produce a data candidate only. Main must
// select a durable marker and verify the VRS strength root before it constructs
// a PublishedStateId and installs the state/head pair as a live publication.
// SWEGCA: src/tinylm_slicer/mosaic_paper_resident_assimilation.py@3bddcb7:491-535
MainRecoveredGenesis MainOwner::reconstruct_genesis_candidate(
    const journal::JournalStore& selected, const MainCommitMarkerFields& marker,
    const AllocationContext& account) {
    auto pinned = selected.pin_records();
    validate_main_marker_journal_binding(marker, pinned);
    PinnedJournalStateSource source(std::move(pinned));
    auto recovered = recover_genesis_state(source, selected.identity_.value(),
                                           marker.state_head, account);
    auto current = make_initial_state(recovered.input(), account);
    if (current->content_digest().bytes() != recovered.content_digest())
        throw std::invalid_argument("main_recovered_content_mismatch");
    return MainRecoveredGenesis{std::move(current), recovered.content_digest(),
                                recovered.publication()};
}

// Only this non-inline member exercises Main's private construction rights.
// The non-member storage type only receives already constructed objects.
// SWEGCA: paper/swegca/ARCHITECTURE_SPEC.md@5901a5a:103-107
MainOwner::MainOwner(MainInitialState initial, AllocationContext account) {
    auto lifetime = std::allocate_shared<MainLifetime>(account.allocator<MainLifetime>());
    auto authority_allocator = account.allocator<MainAuthorityLedger>();
    auto* raw_authority = authority_allocator.allocate(1);
    try {
        // Construction remains inside Main's private authority boundary.
        ::new (static_cast<void*>(raw_authority)) MainAuthorityLedger(account);
    } catch (...) {
        authority_allocator.deallocate(raw_authority, 1);
        throw;
    }
    auto authority = std::shared_ptr<MainAuthorityLedger>(
        raw_authority,
        [authority_allocator](MainAuthorityLedger* value) mutable noexcept {
            value->~MainAuthorityLedger();
            authority_allocator.deallocate(value, 1);
        }, account.allocator<MainAuthorityLedger>());
    auto current = make_initial_state(initial, account);

    auto state_allocator = account.allocator<detail::MainOwnerState>();
    state_ = std::allocate_shared<detail::MainOwnerState>(state_allocator,
        std::move(lifetime), std::move(account), std::move(authority),
        std::move(current));
}

// SWEGCA: paper/swegca/ARCHITECTURE_SPEC.md@5901a5a:103-107
MainOwner::~MainOwner() = default;

// SWEGCA: paper/swegca/ARCHITECTURE_SPEC.md@5901a5a:103-107
StateSnapshot MainOwner::snapshot() const {
    const auto pair = state_->published.load();
    if (!pair)
        throw std::logic_error("main_state_unpublished");
    return StateSnapshot(pair->state, pair->head, state_->lifetime);
}

}  // namespace swegca::vrs
