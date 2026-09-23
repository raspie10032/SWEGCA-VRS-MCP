#include "swegca_architecture/cognitive_state.hpp"

#include "swegca_architecture/sha256.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace swegca::architecture {
namespace {

// Canonical state fields use fixed order, little-endian integers and explicit
// lengths. The domain tag separates this digest from every other SWEGCA hash.
// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3f:567-572
void hash_u8(Sha256& hash, std::uint8_t value) {
    const std::array bytes{static_cast<std::byte>(value)};
    hash.update(bytes);
}

// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3f:567-572
void hash_u64(Sha256& hash, std::uint64_t value) {
    std::array<std::byte, 8> bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index)
        bytes[index] = static_cast<std::byte>((value >> (index * 8)) & 0xff);
    hash.update(bytes);
}

// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3f:567-572
void hash_bytes(Sha256& hash, std::span<const std::byte> bytes) {
    hash_u64(hash, bytes.size());
    hash.update(bytes);
}

// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3f:567-572
void hash_text(Sha256& hash, std::string_view text) {
    hash_u64(hash, text.size());
    hash.update(text);
}

// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:269-277
void hash_tensor(Sha256& hash, std::uint8_t partition,
                 const CognitiveTensor& tensor) {
    hash_u8(hash, partition);
    hash_u8(hash, static_cast<std::uint8_t>(tensor.scalar_type()));
    hash_u8(hash, static_cast<std::uint8_t>(tensor.byte_order()));
    hash_u64(hash, tensor.shape().batches);
    hash_u64(hash, tensor.shape().slots);
    hash_u64(hash, tensor.shape().width);
    hash_bytes(hash, tensor.bytes());
}

// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:83-93
EvidenceReferences canonical_evidence(EvidenceReferences addresses) {
    std::sort(addresses.begin(), addresses.end(),
              [](const ExperienceAddress& left,
                 const ExperienceAddress& right) {
                  return left.value() < right.value();
              });
    if (std::adjacent_find(addresses.begin(), addresses.end()) != addresses.end())
        throw std::invalid_argument("cognitive_state_duplicate_evidence_reference");
    return addresses;
}

// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3f:567-572
Digest256 state_digest(
    const OwnerId& owner, std::uint64_t ordinal, const RoleRegistry& roles,
    const CognitiveTensor& semantic, const CognitiveTensor& executive,
    const CognitiveTensor& scratch, const StructuredWorldGraph& graph,
    std::span<const ExperienceAddress> evidence, const GoalState& goals,
    const ValueState& values, const SelfState& self) {
    Sha256 hash;
    hash.update("swegca.cognitive_state.v1");
    hash_text(hash, owner.value());
    hash_u64(hash, ordinal);

    hash_u64(hash, roles.size());
    for (const auto& role : roles.definitions()) {
        hash_text(hash, role.id.value());
        hash_u8(hash, static_cast<std::uint8_t>(role.partition));
        hash_u64(hash, role.slot);
    }
    hash_tensor(hash, 1, semantic);
    hash_tensor(hash, 2, executive);
    hash_tensor(hash, 3, scratch);

    hash_u64(hash, graph.entities().size());
    for (const auto& entity : graph.entities()) {
        hash_text(hash, entity.id.value());
        hash_text(hash, entity.kind.value());
        hash_bytes(hash, entity.attributes.bytes());
    }
    hash_u64(hash, graph.relations().size());
    for (const auto& relation : graph.relations()) {
        hash_text(hash, relation.id.value());
        hash_text(hash, relation.kind.value());
        hash_text(hash, relation.source.value());
        hash_text(hash, relation.target.value());
        hash_bytes(hash, relation.attributes.bytes());
    }

    hash_u64(hash, evidence.size());
    for (const auto& address : evidence) hash_text(hash, address.value());
    hash_bytes(hash, goals.payload().bytes());
    hash_bytes(hash, values.payload().bytes());
    hash_bytes(hash, self.payload().bytes());
    return Digest256(hash.finish());
}

// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:222-230
std::uint64_t next_ordinal(const CognitiveState& prior) {
    if (prior.generation().ordinal() ==
        std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("successor_state_generation_exhausted");
    return prior.generation().ordinal() + 1;
}

}  // namespace

// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:222-230
void InitialStateKey::consume() {
    if (!std::exchange(valid_, false))
        throw std::logic_error("initial_state_key_already_consumed");
}

// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:222-230
void SuccessorStateKey::consume() {
    if (!std::exchange(valid_, false))
        throw std::logic_error("successor_state_key_already_consumed");
}

// Callers define the schema of each payload and must supply its already
// canonical bytes. The caller's allocation context enforces its injected
// budget when these bytes are allocated.
// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:83-93
CanonicalPayload::CanonicalPayload(const AllocationContext& account,
                                   std::span<const std::byte> bytes)
    : bytes_(account.allocator<std::byte>()) {
    if (!bytes.empty()) bytes_.assign(bytes.begin(), bytes.end());
}

// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:83-93
StructuredWorldGraph::StructuredWorldGraph(
    const AllocationContext& account,
    std::span<const WorldEntityInput> entities,
    std::span<const WorldRelationInput> relations)
    : entities_(account.allocator<WorldEntity>()),
      relations_(account.allocator<WorldRelation>()) {
    entities_.reserve(entities.size());
    relations_.reserve(relations.size());
    for (const auto& input : entities)
        entities_.push_back({EntityId(account, input.id), EntityKind(account, input.kind),
                             CanonicalPayload(account, input.attributes)});
    for (const auto& input : relations)
        relations_.push_back({RelationId(account, input.id), RelationKind(account, input.kind),
                              EntityId(account, input.source), EntityId(account, input.target),
                              CanonicalPayload(account, input.attributes)});
    std::sort(entities_.begin(), entities_.end(),
              [](const WorldEntity& left, const WorldEntity& right) {
                  return left.id.value() < right.id.value();
              });
    std::sort(relations_.begin(), relations_.end(),
              [](const WorldRelation& left, const WorldRelation& right) {
                  return left.id.value() < right.id.value();
              });
    for (std::size_t index = 1; index < entities_.size(); ++index)
        if (entities_[index - 1].id == entities_[index].id)
            throw std::invalid_argument("world_graph_duplicate_entity");

    for (std::size_t index = 0; index < relations_.size(); ++index) {
        const auto& relation = relations_[index];
        if (index != 0 && relations_[index - 1].id == relation.id)
            throw std::invalid_argument("world_graph_duplicate_relation");
        const auto has_entity = [this](const EntityId& id) {
            const auto found = std::lower_bound(
                entities_.begin(), entities_.end(), id.value(),
                [](const WorldEntity& entity, std::string_view value) {
                    return entity.id.value() < value;
                });
            return found != entities_.end() && found->id == id;
        };
        if (!has_entity(relation.source) || !has_entity(relation.target))
            throw std::invalid_argument("world_graph_relation_entity_missing");
    }
}

// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:83-93
CognitiveState::CognitiveState(
    InitialStateKey key, OwnerId owner,
    RoleRegistry roles, CognitiveTensor semantic, CognitiveTensor executive,
    CognitiveTensor scratch, StructuredWorldGraph world_graph,
    EvidenceReferences evidence_references,
    GoalState goals, ValueState values, SelfState self)
    : owner_(std::move(owner)), roles_(std::move(roles)),
      semantic_(std::move(semantic)),
      executive_(std::move(executive)), scratch_(std::move(scratch)),
      world_graph_(std::move(world_graph)),
      evidence_references_(canonical_evidence(std::move(evidence_references))),
      // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:83-93
      goals_(std::move(goals)), values_(std::move(values)),
      self_(std::move(self)),
      generation_((key.consume(), validated_generation(nullptr))) {}

// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:83-93
CognitiveState::CognitiveState(
    SuccessorStateKey key, const CognitiveState& prior,
    RoleRegistry roles, CognitiveTensor semantic, CognitiveTensor executive,
    CognitiveTensor scratch, StructuredWorldGraph world_graph,
    EvidenceReferences evidence_references,
    GoalState goals, ValueState values, SelfState self)
    : owner_(prior.owner_), roles_(std::move(roles)),
      semantic_(std::move(semantic)),
      executive_(std::move(executive)), scratch_(std::move(scratch)),
      world_graph_(std::move(world_graph)),
      evidence_references_(canonical_evidence(std::move(evidence_references))),
      // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:83-93
      goals_(std::move(goals)), values_(std::move(values)),
      self_(std::move(self)),
      generation_((key.consume(), validated_generation(&prior))) {}

// All members read here precede generation_ in declaration order. Validate
// their bounds and continuity before hashing; no provisional digest escapes.
// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:83-93
StateGeneration CognitiveState::validated_generation(
    const CognitiveState* prior) const {
    validate();
    if (prior == nullptr) {
        if (!roles_.matches_initial_profile({
            semantic_.shape().slots, executive_.shape().slots,
            scratch_.shape().slots}))
            throw std::invalid_argument("initial_role_registry_shape_mismatch");
    } else {
        // A guarded write may promote all three partitions; an exact rollback
        // may restore their earlier dtype. The writer checks the operation's
        // dtype rule, while validate() checks their common successor dtype.
        if (semantic_.shape().width != prior->semantic_.shape().width)
            throw std::invalid_argument("successor_tensor_width_changed");
        if (semantic_.shape().slots < prior->semantic_.shape().slots ||
            executive_.shape().slots < prior->executive_.shape().slots ||
            scratch_.shape().slots < prior->scratch_.shape().slots)
            throw std::invalid_argument("successor_tensor_capacity_shrank");
        const auto previous_roles = prior->roles_.definitions();
        const auto next_roles = roles_.definitions();
        if (next_roles.size() < previous_roles.size() ||
            !std::equal(previous_roles.begin(), previous_roles.end(), next_roles.begin()))
            throw std::invalid_argument("successor_role_registry_not_append_only");
    }
    const auto ordinal = prior == nullptr ? 0 : next_ordinal(*prior);
    return StateGeneration(ordinal, state_digest(
        owner_, ordinal, roles_, semantic_, executive_, scratch_, world_graph_,
        evidence_references_, goals_, values_, self_));
}

// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:83-93
void CognitiveState::validate() const {
    const auto& semantic_shape = semantic_.shape();
    const auto& executive_shape = executive_.shape();
    const auto& scratch_shape = scratch_.shape();
    if (semantic_shape.batches != 1 || executive_shape.batches != 1 ||
        scratch_shape.batches != 1)
        throw std::invalid_argument("persistent_state_count_must_be_one");
    if (semantic_shape.width != executive_shape.width ||
        semantic_shape.width != scratch_shape.width)
        throw std::invalid_argument("cognitive_state_width_mismatch");
    if (semantic_.scalar_type() != executive_.scalar_type() ||
        semantic_.scalar_type() != scratch_.scalar_type())
        throw std::invalid_argument("cognitive_state_scalar_type_mismatch");
    std::uint64_t logical_bytes = 0;
    const auto account = [&logical_bytes](std::uint64_t bytes) {
        if (bytes > std::numeric_limits<std::uint64_t>::max() - logical_bytes)
            throw std::overflow_error("cognitive_state_logical_byte_count_overflow");
        logical_bytes += bytes;
    };
    account(semantic_.byte_count());
    account(executive_.byte_count());
    account(scratch_.byte_count());
    account(owner_.value().size());
    for (const auto& role : roles_.definitions()) account(role.id.value().size());
    for (const auto& entity : world_graph_.entities()) {
        account(entity.id.value().size());
        account(entity.kind.value().size());
        account(entity.attributes.bytes().size());
    }
    for (const auto& relation : world_graph_.relations()) {
        account(relation.id.value().size());
        account(relation.kind.value().size());
        account(relation.source.value().size());
        account(relation.target.value().size());
        account(relation.attributes.bytes().size());
    }
    for (const auto& address : evidence_references_)
        account(address.value().size());
    account(goals_.payload().bytes().size());
    account(values_.payload().bytes().size());
    account(self_.payload().bytes().size());

    for (const auto& definition : roles_.definitions()) {
        std::uint64_t capacity = 0;
        switch (definition.partition) {
            case TensorPartition::semantic:
                capacity = semantic_shape.slots;
                break;
            case TensorPartition::executive:
                capacity = executive_shape.slots;
                break;
            case TensorPartition::scratch:
                capacity = scratch_shape.slots;
                break;
        }
        if (definition.slot >= capacity)
            throw std::invalid_argument("role_registry_slot_outside_state");
    }

}

// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:222-230
StateSnapshot::StateSnapshot(std::shared_ptr<const CognitiveState> state,
                             std::shared_ptr<const void> main_lifetime)
    : main_lifetime_(std::move(main_lifetime)), state_(std::move(state)) {
    if (!state_ || !main_lifetime_)
        throw std::invalid_argument("state_snapshot_must_not_be_null");
}

// Keep both the old and the replacement Main leases alive while swapping
// snapshots. The old state's last reference is released before its lease.
// SWEGCA: user@2026-09-23:1
StateSnapshot& StateSnapshot::operator=(StateSnapshot other) noexcept {
    state_.swap(other.state_);
    main_lifetime_.swap(other.main_lifetime_);
    return *this;
}

// Moving a snapshot transfers its ownership; the emptied handle cannot expose
// a state. Reject it explicitly instead of dereferencing an empty shared_ptr.
// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:222-230
const CognitiveState& StateSnapshot::state() const {
    if (!state_) throw std::logic_error("state_snapshot_not_live");
    return *state_;
}

}  // namespace swegca::architecture
