#include "swegca_vrs/cognitive_state.hpp"

#include "swegca_vrs/core_sha256.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

namespace swegca::vrs {
namespace {

// Weak source analogy: the author's hash includes tensors and metadata. This
// native binary stream uses fixed order, little-endian integers and explicit
// lengths, so its bytes and digest differ from the Python JSON hash. The
// provisional generation ordinal is outside this C++ content digest.
// SWEGCA: src/swegca/mosaic_bounded_world_write.py@5901a5a:262-283
void emit_u8(StateContentSink write, std::uint8_t value) {
    const std::array bytes{static_cast<std::byte>(value)};
    write(bytes);
}

// SWEGCA: src/swegca/mosaic_bounded_world_write.py@5901a5a:262-283
void emit_u64(StateContentSink write, std::uint64_t value) {
    std::array<std::byte, 8> bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index)
        bytes[index] = static_cast<std::byte>((value >> (index * 8)) & 0xff);
    write(bytes);
}

// SWEGCA: src/swegca/mosaic_bounded_world_write.py@5901a5a:262-283
void emit_bytes(StateContentSink write, std::span<const std::byte> bytes) {
    emit_u64(write, bytes.size());
    write(bytes);
}

// SWEGCA: src/swegca/mosaic_bounded_world_write.py@5901a5a:262-283
void emit_text(StateContentSink write, std::string_view text) {
    emit_u64(write, text.size());
    write(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(text.data()), text.size()));
}

// SWEGCA: src/swegca/mosaic_bounded_world_write.py@5901a5a:262-283
void emit_tensor(StateContentSink write, std::uint8_t partition,
                 const CognitiveTensor& tensor) {
    emit_u8(write, partition);
    emit_u8(write, static_cast<std::uint8_t>(tensor.scalar_type()));
    emit_u8(write, static_cast<std::uint8_t>(tensor.byte_order()));
    emit_u64(write, tensor.shape().batches);
    emit_u64(write, tensor.shape().slots);
    emit_u64(write, tensor.shape().width);
    emit_u64(write, tensor.byte_count());
    tensor.for_each_chunk([write](std::span<const std::byte> chunk) {
        write(chunk);
    });
}

// The emitted stream is this C++ state's domain-separated digest preimage.
// A persistent writer consumes the same bytes through a bounded sink; the
// original Python function uses a different tensor/JSON encoding.
// SWEGCA: src/swegca/mosaic_bounded_world_write.py@5901a5a:262-283
void emit_state_content(
    StateContentSink write,
    const OwnerId& owner, const RoleRegistry& roles,
    const CognitiveTensor& semantic, const CognitiveTensor& executive,
    const CognitiveTensor& scratch, const StructuredWorldGraph& graph,
    std::span<const ExperienceAddress> evidence, const GoalState& goals,
    const ValueState& values, const SelfState& self) {
    constexpr std::string_view domain = "swegca.cognitive_state.content.v3";
    write(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(domain.data()), domain.size()));
    emit_text(write, owner.value());

    emit_u64(write, roles.size());
    for (const auto& role : roles.definitions()) {
        emit_text(write, role.id.value());
        emit_u8(write, static_cast<std::uint8_t>(role.partition));
        emit_u64(write, role.slot);
    }
    emit_tensor(write, 1, semantic);
    emit_tensor(write, 2, executive);
    emit_tensor(write, 3, scratch);

    emit_u64(write, graph.entities().size());
    for (const auto& entity : graph.entities()) {
        emit_text(write, entity.id.value());
        emit_text(write, entity.kind.value());
        emit_bytes(write, entity.properties.bytes());
        emit_bytes(write, entity.spatial.bytes());
        emit_u64(write, entity.evidence_references.size());
        for (const auto& address : entity.evidence_references)
            emit_text(write, address.value());
    }
    emit_u64(write, graph.relations().size());
    for (const auto& relation : graph.relations()) {
        emit_text(write, relation.subject.value());
        emit_text(write, relation.predicate.value());
        emit_text(write, relation.object.value());
        emit_bytes(write, relation.properties.bytes());
    }

    emit_u64(write, evidence.size());
    for (const auto& address : evidence) emit_text(write, address.value());
    emit_bytes(write, goals.payload().bytes());
    emit_bytes(write, values.payload().bytes());
    emit_bytes(write, self.payload().bytes());
    emit_u8(write, self.write_head().has_value() ? 1 : 0);
    if (self.write_head()) {
        const auto& head = *self.write_head();
        emit_text(write, head.policy_version.value());
        write(head.receipt_id.bytes());
        emit_u64(write, head.revision);
        emit_text(write, head.target_role.value());
        emit_u64(write, head.evidence_references.size());
        for (const auto& address : head.evidence_references)
            emit_text(write, address.value());
    }
}

// SWEGCA: src/swegca/mosaic_bounded_world_write.py@5901a5a:262-283
Digest256 state_digest(
    const OwnerId& owner, const RoleRegistry& roles,
    const CognitiveTensor& semantic, const CognitiveTensor& executive,
    const CognitiveTensor& scratch, const StructuredWorldGraph& graph,
    std::span<const ExperienceAddress> evidence, const GoalState& goals,
    const ValueState& values, const SelfState& self) {
    Sha256 hash;
    const auto feed_hash = [&hash](std::span<const std::byte> bytes) {
        hash.update(bytes);
    };
    emit_state_content(StateContentSink(feed_hash), owner, roles, semantic,
                       executive, scratch, graph, evidence, goals, values, self);
    return Digest256(hash.finish());
}

// Weak source analogy: the author requires one authoritative state, but has
// no matching numeric successor ordinal. This counter is C++ infrastructure.
// SWEGCA: paper/swegca/ARCHITECTURE_SPEC.md@5901a5a:103-107
std::uint64_t next_ordinal(const CognitiveState& prior) {
    if (prior.generation().ordinal() ==
        std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("successor_state_generation_exhausted");
    return prior.generation().ordinal() + 1;
}

}  // namespace

// SWEGCA: paper/swegca/ARCHITECTURE_SPEC.md@5901a5a:103-107
void InitialStateKey::consume() {
    if (!std::exchange(valid_, false))
        throw std::logic_error("initial_state_key_already_consumed");
}

// SWEGCA: paper/swegca/ARCHITECTURE_SPEC.md@5901a5a:103-107
void SuccessorStateKey::consume() {
    if (!std::exchange(valid_, false))
        throw std::logic_error("successor_state_key_already_consumed");
}

// Callers define each metadata mapping's schema and canonical byte encoding.
// The source uses JSON mappings; this native payload does not parse them.
// The caller's allocation context enforces its injected budget.
// SWEGCA: src/swegca/mosaic_cognitive_kernel.py@5901a5a:220-254
CanonicalPayload::CanonicalPayload(const AllocationContext& account,
                                   std::span<const std::byte> bytes)
    : bytes_(account.allocator<std::byte>()) {
    if (!bytes.empty()) bytes_.assign(bytes.begin(), bytes.end());
}

// SWEGCA: src/swegca/mosaic_cognitive_kernel.py@5901a5a:107-160
StructuredWorldGraph::StructuredWorldGraph(
    const AllocationContext& account,
    std::span<const WorldEntityInput> entities,
    std::span<const WorldRelationInput> relations)
    : entities_(account.allocator<WorldEntity>()),
      relations_(account.allocator<WorldRelation>()) {
    entities_.reserve(entities.size());
    relations_.reserve(relations.size());
    std::set<std::string_view, std::less<>, AllocationAdapter<std::string_view>> ids(
        std::less<>{}, account.allocator<std::string_view>());
    for (const auto& input : entities) {
        // The source rejects only an empty per-entity evidence ref. Native
        // ExperienceAddress also rejects blank, NUL, invalid UTF-8 and texts
        // over its byte limit; this narrower address rule is still visible.
        EvidenceReferences references(account.allocator<ExperienceAddress>());
        references.reserve(input.evidence_references.size());
        for (const auto address : input.evidence_references)
            references.emplace_back(account, address);
        entities_.push_back({EntityId(account, input.id), EntityKind(account, input.kind),
                             CanonicalPayload(account, input.properties),
                             CanonicalPayload(account, input.spatial),
                             std::move(references)});
        if (!ids.emplace(entities_.back().id.value()).second)
            throw std::invalid_argument("world_graph_duplicate_entity");
    }
    for (const auto& input : relations) {
        if (!ids.contains(input.subject) || !ids.contains(input.object))
            throw std::invalid_argument("world_graph_relation_entity_missing");
        relations_.push_back({EntityId(account, input.subject),
                              RelationPredicate(account, input.predicate),
                              EntityId(account, input.object),
                              CanonicalPayload(account, input.properties)});
    }
}

// SWEGCA: src/swegca/mosaic_cognitive_kernel.py@5901a5a:220-254
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
      evidence_references_(std::move(evidence_references)),
      // Preserve original evidence reference order and repeats.
      // SWEGCA: src/swegca/mosaic_cognitive_kernel.py@5901a5a:220-254
      goals_(std::move(goals)), values_(std::move(values)),
      self_(std::move(self)),
      generation_((key.consume(), validated_generation(nullptr))) {}

// SWEGCA: src/swegca/mosaic_cognitive_kernel.py@5901a5a:220-254
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
      evidence_references_(std::move(evidence_references)),
      // Preserve original evidence reference order and repeats.
      // SWEGCA: src/swegca/mosaic_cognitive_kernel.py@5901a5a:220-254
      goals_(std::move(goals)), values_(std::move(values)),
      self_(std::move(self)),
      generation_((key.consume(), validated_generation(&prior))) {}

// All members read here precede generation_ in declaration order. Validate
// before hashing. Ordinal continuity and append-only roles are C++ successor
// rules, not checks in the original CognitiveState constructor.
// SWEGCA: src/swegca/mosaic_cognitive_kernel.py@5901a5a:220-254
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
        owner_, roles_, semantic_, executive_, scratch_, world_graph_,
        evidence_references_, goals_, values_, self_));
}

// This C++ stream is the same canonical preimage used above to compute the
// content digest; a state-part writer can consume it without a whole-state copy.
// SWEGCA: src/swegca/mosaic_bounded_world_write.py@5901a5a:262-283
void CognitiveState::for_each_content_chunk(StateContentSink write) const {
    emit_state_content(write, owner_, roles_, semantic_, executive_, scratch_,
                       world_graph_, evidence_references_, goals_, values_, self_);
}

// The original state accepts any common batch dimension, including zero. The
// separate guarded writer currently accepts only batch one; that narrower
// operation must not narrow the general state itself.
// SWEGCA: src/swegca/mosaic_cognitive_kernel.py@5901a5a:234-254
void CognitiveState::validate() const {
    const auto& semantic_shape = semantic_.shape();
    const auto& executive_shape = executive_.shape();
    const auto& scratch_shape = scratch_.shape();
    if (semantic_shape.batches != executive_shape.batches ||
        semantic_shape.batches != scratch_shape.batches)
        throw std::invalid_argument("cognitive_state_batch_mismatch");
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
        account(entity.properties.bytes().size());
        account(entity.spatial.bytes().size());
        for (const auto& address : entity.evidence_references)
            account(address.value().size());
    }
    for (const auto& relation : world_graph_.relations()) {
        account(relation.subject.value().size());
        account(relation.predicate.value().size());
        account(relation.object.value().size());
        account(relation.properties.bytes().size());
    }
    for (const auto& address : evidence_references_)
        account(address.value().size());
    account(goals_.payload().bytes().size());
    account(values_.payload().bytes().size());
    account(self_.payload().bytes().size());
    if (self_.write_head()) {
        const auto& write = *self_.write_head();
        if (write.revision == 0)
            throw std::invalid_argument("bounded_write_revision_invalid");
        account(write.policy_version.value().size());
        account(write.target_role.value().size());
        for (const auto& address : write.evidence_references)
            account(address.value().size());
    }

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

// SWEGCA: paper/swegca/ARCHITECTURE_SPEC.md@5901a5a:103-107
StateSnapshot::StateSnapshot(std::shared_ptr<const CognitiveState> state,
                             std::shared_ptr<const void> main_lifetime)
    : main_lifetime_(std::move(main_lifetime)), state_(std::move(state)) {
    if (!state_ || !main_lifetime_)
        throw std::invalid_argument("state_snapshot_must_not_be_null");
}

// Keep both the old and the replacement Main leases alive while swapping
// snapshots. The old state's last reference is released before its lease.
// Weak source analogy: the source requires one Main-owned state, while this
// swap's reference-release order is native C++ lease infrastructure.
// SWEGCA: paper/swegca/ARCHITECTURE_SPEC.md@5901a5a:103-107
StateSnapshot& StateSnapshot::operator=(StateSnapshot other) noexcept {
    state_.swap(other.state_);
    main_lifetime_.swap(other.main_lifetime_);
    return *this;
}

// Moving a snapshot transfers its ownership; the emptied handle cannot expose
// a state. Reject it explicitly instead of dereferencing an empty shared_ptr.
// SWEGCA: paper/swegca/ARCHITECTURE_SPEC.md@5901a5a:103-107
const CognitiveState& StateSnapshot::state() const {
    if (!state_) throw std::logic_error("state_snapshot_not_live");
    return *state_;
}

}  // namespace swegca::vrs
