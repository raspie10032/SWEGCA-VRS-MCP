#include "swegca_vrs/cognitive_state.hpp"

#include "swegca_vrs/cognition.hpp"

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
// publication identity is outside this C++ content digest.
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
void emit_tensor(StateContentSink write, const StateContentSectionSink* section,
                 std::uint8_t partition,
                 const CognitiveTensor& tensor) {
    emit_u8(write, partition);
    emit_u8(write, static_cast<std::uint8_t>(tensor.scalar_type()));
    emit_u8(write, static_cast<std::uint8_t>(tensor.byte_order()));
    emit_u64(write, tensor.shape().batches);
    emit_u64(write, tensor.shape().slots);
    emit_u64(write, tensor.shape().width);
    emit_u64(write, tensor.byte_count());
    if (section) (*section)(StateContentSection::tensor_chunks);
    tensor.for_each_chunk([write](std::span<const std::byte> chunk) {
        write(chunk);
    });
}

// The emitted stream is this C++ state's domain-separated digest preimage.
// A persistent writer consumes the same bytes through a bounded sink; the
// original Python function uses a different tensor/JSON encoding.
// SWEGCA: src/swegca/mosaic_bounded_world_write.py@5901a5a:262-283
void emit_state_prefix(
    StateContentSink write, const StateContentSectionSink* section,
    const OwnerId& owner, const RoleRegistry& roles,
    const CognitiveTensor& semantic, const CognitiveTensor& executive,
    const CognitiveTensor& scratch, const StructuredWorldGraph& graph,
    std::span<const ExperienceAddress> evidence, const GoalState& goals,
    const ValueState& values, const SelfState& self) {
    if (section) (*section)(StateContentSection::prefix);
    // v4: the bounded-write head carries the claim and proposal digest.
    // v5: the final fields end with the autonomy control's presence and bytes.
    constexpr std::string_view domain = "swegca.cognitive_state.content.v5";
    write(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(domain.data()), domain.size()));
    emit_text(write, owner.value());

    emit_u64(write, roles.size());
    for (const auto& role : roles.definitions()) {
        emit_text(write, role.id.value());
        emit_u8(write, static_cast<std::uint8_t>(role.partition));
        emit_u64(write, role.slot);
    }
    if (section) (*section)(StateContentSection::semantic_tensor);
    emit_tensor(write, section, 1, semantic);
    if (section) (*section)(StateContentSection::executive_tensor);
    emit_tensor(write, section, 2, executive);
    if (section) (*section)(StateContentSection::scratch_tensor);
    emit_tensor(write, section, 3, scratch);

    if (section) (*section)(StateContentSection::entities);
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
    if (section) (*section)(StateContentSection::relations);
    emit_u64(write, graph.relations().size());
    for (const auto& relation : graph.relations()) {
        emit_text(write, relation.subject.value());
        emit_text(write, relation.predicate.value());
        emit_text(write, relation.object.value());
        emit_bytes(write, relation.properties.bytes());
    }

    if (section) (*section)(StateContentSection::evidence);
    emit_u64(write, evidence.size());
    for (const auto& address : evidence) emit_text(write, address.value());
    if (section) (*section)(StateContentSection::final_fields);
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
        emit_text(write, head.claim.claim().value());
        emit_u64(write, head.claim.revision());
        write(head.proposal_digest.bytes());
    }
}

// The final control is the only section an autonomy phase changes. Keeping
// this suffix separate lets a successor share the verified immutable prefix
// without reading all World tensor bytes again.
// SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:167-186
void emit_autonomy_suffix(StateContentSink write,
                          const std::optional<AutonomyState>& autonomy) {
    emit_u8(write, autonomy.has_value() ? 1 : 0);
    if (autonomy) emit_bytes(write, autonomy->bytes());
}

}  // namespace

// The World and the non-autonomy metadata are immutable between autonomous
// phases. One account-charged body is shared by those successors, so neither
// its tensors, graph nor metadata are copied on a phase step. The unfinished
// hash is exactly the v5 content stream before the autonomy presence byte;
// it is a derived in-memory cache, never a publication or authority token.
// SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:167-186
struct CognitiveState::StateBody final {
    StateBody(OwnerId owner_in, RoleRegistry roles_in,
              CognitiveTensor semantic_in, CognitiveTensor executive_in,
              CognitiveTensor scratch_in, StructuredWorldGraph graph_in,
              EvidenceReferences evidence_in, GoalState goals_in,
              ValueState values_in, SelfState self_in)
        // SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:167-186
        : owner(std::move(owner_in)), roles(std::move(roles_in)),
          semantic(std::move(semantic_in)), executive(std::move(executive_in)),
          scratch(std::move(scratch_in)), graph(std::move(graph_in)),
          evidence(std::move(evidence_in)), goals(std::move(goals_in)),
          values(std::move(values_in)), self(std::move(self_in)) {}

    OwnerId owner;
    RoleRegistry roles;
    CognitiveTensor semantic;
    CognitiveTensor executive;
    CognitiveTensor scratch;
    StructuredWorldGraph graph;
    EvidenceReferences evidence;
    GoalState goals;
    ValueState values;
    SelfState self;
    // Filled after cross-field validation, before this body is shared.
    mutable std::optional<Sha256> prefix_hash;
};

namespace {

// Finishes the exact canonical v5 stream from a checked immutable prefix.
// SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:167-186
Digest256 digest_with_autonomy(const Sha256& prefix,
                               const std::optional<AutonomyState>& autonomy) {
    Sha256 hash = prefix;
    const auto feed = [&hash](std::span<const std::byte> bytes) {
        hash.update(bytes);
    };
    emit_autonomy_suffix(StateContentSink(feed), autonomy);
    return Digest256(hash.finish());
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

// The bytes pass the same decode check a recovered control does, so a state
// never holds a control that its own control() or cold recovery would refuse.
// SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:167-186
AutonomyState AutonomyState::encode(const AllocationContext& account,
                                    const AutonomyControl& control) {
    const auto bytes = control.encode(account);
    return decode(account, std::span<const std::byte>(bytes.data(), bytes.size()));
}

// Decode then re-encode: a second byte form of the same control would give
// one logical state two content digests.
// SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:167-186
AutonomyState AutonomyState::decode(const AllocationContext& account,
                                    std::span<const std::byte> bytes) {
    const auto control = AutonomyControl::decode(account, bytes);
    const auto canonical = control.encode(account);
    if (canonical.size() != bytes.size() ||
        !std::equal(canonical.begin(), canonical.end(), bytes.begin()))
        throw std::invalid_argument("autonomy_state_not_canonical");
    return AutonomyState(CanonicalPayload(account, bytes));
}

// SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:150-186
AutonomyControl AutonomyState::control(const AllocationContext& account) const {
    return AutonomyControl::decode(account, payload_.bytes());
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
    InitialStateKey key, const AllocationContext& memory, OwnerId owner,
    RoleRegistry roles, CognitiveTensor semantic, CognitiveTensor executive,
    CognitiveTensor scratch, StructuredWorldGraph world_graph,
    EvidenceReferences evidence_references,
    GoalState goals, ValueState values, SelfState self,
    std::optional<AutonomyState> autonomy)
    : body_(std::allocate_shared<StateBody>(
          memory.allocator<StateBody>(), std::move(owner), std::move(roles),
          std::move(semantic), std::move(executive), std::move(scratch),
          std::move(world_graph), std::move(evidence_references),
          std::move(goals), std::move(values), std::move(self))),
      autonomy_(std::move(autonomy)),
      // SWEGCA: src/swegca/mosaic_cognitive_kernel.py@5901a5a:220-254
      content_digest_((key.consume(), validated_content_digest(nullptr))) {}

// SWEGCA: src/swegca/mosaic_cognitive_kernel.py@5901a5a:220-254
CognitiveState::CognitiveState(
    SuccessorStateKey key, const AllocationContext& memory,
    const CognitiveState& prior,
    RoleRegistry roles, CognitiveTensor semantic, CognitiveTensor executive,
    CognitiveTensor scratch, StructuredWorldGraph world_graph,
    EvidenceReferences evidence_references,
    GoalState goals, ValueState values, SelfState self)
    : body_(std::allocate_shared<StateBody>(
          memory.allocator<StateBody>(), prior.owner(), std::move(roles),
          std::move(semantic), std::move(executive), std::move(scratch),
          std::move(world_graph), std::move(evidence_references),
          std::move(goals), std::move(values), std::move(self))),
      autonomy_(prior.autonomy_),
      // SWEGCA: src/swegca/mosaic_cognitive_kernel.py@5901a5a:220-254
      content_digest_((key.consume(), validated_content_digest(&prior))) {}

// Shares the exact immutable body, including tensor objects and its verified
// prefix hash. Only Main's one-use successor key can make this state.
// SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:167-186
CognitiveState::CognitiveState(SuccessorStateKey key,
                               const CognitiveState& prior,
                               AutonomyState next_autonomy)
    : body_(prior.body_), autonomy_(std::move(next_autonomy)),
      // SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:167-186
      content_digest_((key.consume(), digest_with_autonomy(body_->prefix_hash.value(),
                                                            autonomy_))) {}

// SWEGCA: src/swegca/mosaic_cognitive_kernel.py@5901a5a:220-254
const OwnerId& CognitiveState::owner() const noexcept { return body_->owner; }
// SWEGCA: src/swegca/mosaic_cognitive_kernel.py@5901a5a:220-254
const RoleRegistry& CognitiveState::roles() const noexcept { return body_->roles; }
// SWEGCA: src/swegca/mosaic_cognitive_kernel.py@5901a5a:220-254
const CognitiveTensor& CognitiveState::semantic() const noexcept { return body_->semantic; }
// SWEGCA: src/swegca/mosaic_cognitive_kernel.py@5901a5a:220-254
const CognitiveTensor& CognitiveState::executive() const noexcept { return body_->executive; }
// SWEGCA: src/swegca/mosaic_cognitive_kernel.py@5901a5a:220-254
const CognitiveTensor& CognitiveState::scratch() const noexcept { return body_->scratch; }
// SWEGCA: src/swegca/mosaic_cognitive_kernel.py@5901a5a:220-254
const StructuredWorldGraph& CognitiveState::world_graph() const noexcept { return body_->graph; }
// SWEGCA: src/swegca/mosaic_cognitive_kernel.py@5901a5a:220-254
std::span<const ExperienceAddress> CognitiveState::evidence_references() const noexcept {
    return body_->evidence;
}
// SWEGCA: src/swegca/mosaic_cognitive_kernel.py@5901a5a:220-254
const GoalState& CognitiveState::goals() const noexcept { return body_->goals; }
// SWEGCA: src/swegca/mosaic_cognitive_kernel.py@5901a5a:220-254
const ValueState& CognitiveState::values() const noexcept { return body_->values; }
// SWEGCA: src/swegca/mosaic_cognitive_kernel.py@5901a5a:220-254
const SelfState& CognitiveState::self() const noexcept { return body_->self; }

// All members read here precede content_digest_ in declaration order.
// Validate before hashing; append-only roles are a C++ successor rule, not
// a check in the original CognitiveState constructor.
// SWEGCA: src/swegca/mosaic_cognitive_kernel.py@5901a5a:220-254
Digest256 CognitiveState::validated_content_digest(
    const CognitiveState* prior) const {
    validate();
    if (prior == nullptr) {
        if (!body_->roles.matches_initial_profile({
            body_->semantic.shape().slots, body_->executive.shape().slots,
            body_->scratch.shape().slots}))
            throw std::invalid_argument("initial_role_registry_shape_mismatch");
    } else {
        // A guarded write may promote all three partitions; an exact rollback
        // may restore their earlier dtype. The writer checks the operation's
        // dtype rule, while validate() checks their common successor dtype.
        if (body_->semantic.shape().width != prior->body_->semantic.shape().width)
            throw std::invalid_argument("successor_tensor_width_changed");
        if (body_->semantic.shape().slots < prior->body_->semantic.shape().slots ||
            body_->executive.shape().slots < prior->body_->executive.shape().slots ||
            body_->scratch.shape().slots < prior->body_->scratch.shape().slots)
            throw std::invalid_argument("successor_tensor_capacity_shrank");
        const auto previous_roles = prior->body_->roles.definitions();
        const auto next_roles = body_->roles.definitions();
        if (next_roles.size() < previous_roles.size() ||
            !std::equal(previous_roles.begin(), previous_roles.end(), next_roles.begin()))
            throw std::invalid_argument("successor_role_registry_not_append_only");
    }
    body_->prefix_hash.emplace();
    const auto feed = [this](std::span<const std::byte> bytes) {
        body_->prefix_hash->update(bytes);
    };
    emit_state_prefix(StateContentSink(feed), nullptr, body_->owner, body_->roles,
                      body_->semantic, body_->executive, body_->scratch,
                      body_->graph, body_->evidence, body_->goals,
                      body_->values, body_->self);
    return digest_with_autonomy(body_->prefix_hash.value(), autonomy_);
}

// This C++ stream is the same canonical preimage used above to compute the
// content digest; a state-part writer can consume it without a whole-state copy.
// SWEGCA: src/swegca/mosaic_bounded_world_write.py@5901a5a:262-283
void CognitiveState::for_each_content_chunk(StateContentSink write) const {
    emit_state_prefix(write, nullptr, body_->owner, body_->roles,
                      body_->semantic, body_->executive, body_->scratch,
                      body_->graph, body_->evidence, body_->goals,
                      body_->values, body_->self);
    emit_autonomy_suffix(write, autonomy_);
}

// The section markers are metadata for a bounded writer, not content bytes.
// SWEGCA: src/swegca/mosaic_bounded_world_write.py@5901a5a:262-283
void CognitiveState::for_each_content_chunk(
    StateContentSink write, StateContentSectionSink section) const {
    emit_state_prefix(write, &section, body_->owner, body_->roles,
                      body_->semantic, body_->executive, body_->scratch,
                      body_->graph, body_->evidence, body_->goals,
                      body_->values, body_->self);
    emit_autonomy_suffix(write, autonomy_);
}

// The original state accepts any common batch dimension, including zero. The
// separate guarded writer currently accepts only batch one; that narrower
// operation must not narrow the general state itself.
// SWEGCA: src/swegca/mosaic_cognitive_kernel.py@5901a5a:234-254
void CognitiveState::validate() const {
    const auto& semantic_shape = body_->semantic.shape();
    const auto& executive_shape = body_->executive.shape();
    const auto& scratch_shape = body_->scratch.shape();
    if (semantic_shape.batches != executive_shape.batches ||
        semantic_shape.batches != scratch_shape.batches)
        throw std::invalid_argument("cognitive_state_batch_mismatch");
    if (semantic_shape.width != executive_shape.width ||
        semantic_shape.width != scratch_shape.width)
        throw std::invalid_argument("cognitive_state_width_mismatch");
    if (body_->semantic.scalar_type() != body_->executive.scalar_type() ||
        body_->semantic.scalar_type() != body_->scratch.scalar_type())
        throw std::invalid_argument("cognitive_state_scalar_type_mismatch");
    std::uint64_t logical_bytes = 0;
    const auto account = [&logical_bytes](std::uint64_t bytes) {
        if (bytes > std::numeric_limits<std::uint64_t>::max() - logical_bytes)
            throw std::overflow_error("cognitive_state_logical_byte_count_overflow");
        logical_bytes += bytes;
    };
    account(body_->semantic.byte_count());
    account(body_->executive.byte_count());
    account(body_->scratch.byte_count());
    account(body_->owner.value().size());
    for (const auto& role : body_->roles.definitions()) account(role.id.value().size());
    for (const auto& entity : body_->graph.entities()) {
        account(entity.id.value().size());
        account(entity.kind.value().size());
        account(entity.properties.bytes().size());
        account(entity.spatial.bytes().size());
        for (const auto& address : entity.evidence_references)
            account(address.value().size());
    }
    for (const auto& relation : body_->graph.relations()) {
        account(relation.subject.value().size());
        account(relation.predicate.value().size());
        account(relation.object.value().size());
        account(relation.properties.bytes().size());
    }
    for (const auto& address : body_->evidence)
        account(address.value().size());
    account(body_->goals.payload().bytes().size());
    account(body_->values.payload().bytes().size());
    account(body_->self.payload().bytes().size());
    if (body_->self.write_head()) {
        const auto& write = *body_->self.write_head();
        if (write.revision == 0)
            throw std::invalid_argument("bounded_write_revision_invalid");
        account(write.policy_version.value().size());
        account(write.target_role.value().size());
        for (const auto& address : write.evidence_references)
            account(address.value().size());
        account(write.claim.claim().value().size());
    }
    if (autonomy_) account(autonomy_->bytes().size());

    for (const auto& definition : body_->roles.definitions()) {
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
                             PublishedStateId head,
                             std::shared_ptr<const void> main_lifetime)
    : main_lifetime_(std::move(main_lifetime)), state_(std::move(state)),
      head_(std::move(head)) {
    if (!state_ || !main_lifetime_)
        throw std::invalid_argument("state_snapshot_must_not_be_null");
    if (state_->content_digest() != head_.content_digest())
        throw std::invalid_argument("state_snapshot_head_content_mismatch");
}

// Keep both the old and the replacement Main leases alive while swapping
// snapshots. The old state's last reference is released before its lease.
// Weak source analogy: the source requires one Main-owned state, while this
// swap's reference-release order is native C++ lease infrastructure.
// SWEGCA: paper/swegca/ARCHITECTURE_SPEC.md@5901a5a:103-107
StateSnapshot& StateSnapshot::operator=(StateSnapshot other) noexcept {
    state_.swap(other.state_);
    main_lifetime_.swap(other.main_lifetime_);
    std::swap(head_, other.head_);
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
