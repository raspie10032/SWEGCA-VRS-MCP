#pragma once

#include "swegca_architecture/authority_roles.hpp"
#include "swegca_architecture/native_tensor.hpp"
#include "swegca_architecture/published_state_id.hpp"
#include "swegca_architecture/role_registry.hpp"
#include "swegca_architecture/strong_types.hpp"

#include <compare>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace swegca::architecture {

class InitialStateKey final {
public:
    InitialStateKey(const InitialStateKey&) = delete;
    // C++ one-use construction gate for the source's single Main owner.
    // SWEGCA: paper/swegca/ARCHITECTURE_SPEC.md@5901a5a:103-107
    InitialStateKey(InitialStateKey&& other) noexcept
        : valid_(std::exchange(other.valid_, false)) {}
    ~InitialStateKey() = default;
    void consume();
private:
    InitialStateKey() = default;
    bool valid_ = true;
    friend class MainOwner;
};

class SuccessorStateKey final {
public:
    SuccessorStateKey(const SuccessorStateKey&) = delete;
    // C++ one-use construction gate; no matching Python key type exists.
    // SWEGCA: paper/swegca/ARCHITECTURE_SPEC.md@5901a5a:103-107
    SuccessorStateKey(SuccessorStateKey&& other) noexcept
        : valid_(std::exchange(other.valid_, false)) {}
    ~SuccessorStateKey() = default;
    void consume();
private:
    SuccessorStateKey() = default;
    bool valid_ = true;
    friend class MainStateWriter;
};

class CanonicalPayload final {
public:
    // Native byte representation of the source's state metadata mappings.
    // A caller supplies its schema and canonical encoding; this type does
    // not parse or validate the original Python JSON mapping.
    CanonicalPayload(const AllocationContext& account,
                     std::span<const std::byte> bytes);

    // SWEGCA: src/swegca/mosaic_cognitive_kernel.py@5901a5a:220-254
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
        return bytes_;
    }

    auto operator<=>(const CanonicalPayload&) const = default;

private:
    std::vector<std::byte, AllocationAdapter<std::byte>> bytes_;
};

using EvidenceReferences =
    std::vector<ExperienceAddress, AllocationAdapter<ExperienceAddress>>;

// The author graph keeps properties, spatial data and per-entity provenance
// separate. CanonicalPayload is their native byte representation; the caller
// supplies each payload's schema and canonical encoding. Native
// ExperienceAddress is stricter than the source's nonempty entity ref check.
struct WorldEntity final {
    EntityId id;
    EntityKind kind;
    CanonicalPayload properties;
    CanonicalPayload spatial;
    EvidenceReferences evidence_references;

    auto operator<=>(const WorldEntity&) const = default;
};

struct WorldRelation final {
    EntityId subject;
    RelationPredicate predicate;
    EntityId object;
    CanonicalPayload properties;

    auto operator<=>(const WorldRelation&) const = default;
};

// Borrowed construction inputs; graph-owned identities, payloads and arrays
// are copied only through Main's allocator.
struct WorldEntityInput final {
    std::string_view id;
    std::string_view kind;
    std::span<const std::byte> properties;
    std::span<const std::byte> spatial;
    std::span<const std::string_view> evidence_references;
};

struct WorldRelationInput final {
    std::string_view subject;
    std::string_view predicate;
    std::string_view object;
    std::span<const std::byte> properties;
};

class StructuredWorldGraph final {
public:
    // Preserve source tuple order, per-entity provenance, and relation
    // endpoints. The original permits repeated relation triples.
    // SWEGCA: src/swegca/mosaic_cognitive_kernel.py@5901a5a:107-160
    StructuredWorldGraph(const AllocationContext& account,
                         std::span<const WorldEntityInput> entities,
                         std::span<const WorldRelationInput> relations);

    // SWEGCA: src/swegca/mosaic_cognitive_kernel.py@5901a5a:107-160
    [[nodiscard]] std::span<const WorldEntity> entities() const noexcept {
        return entities_;
    }
    // SWEGCA: src/swegca/mosaic_cognitive_kernel.py@5901a5a:107-160
    [[nodiscard]] std::span<const WorldRelation> relations() const noexcept {
        return relations_;
    }

private:
    std::vector<WorldEntity, AllocationAdapter<WorldEntity>> entities_;
    std::vector<WorldRelation, AllocationAdapter<WorldRelation>> relations_;
};

template <class Tag>
class StateSection final {
public:
    // SWEGCA: src/swegca/mosaic_cognitive_kernel.py@5901a5a:220-254
    explicit StateSection(CanonicalPayload payload)
        : payload_(std::move(payload)) {}

    // SWEGCA: src/swegca/mosaic_cognitive_kernel.py@5901a5a:220-254
    [[nodiscard]] const CanonicalPayload& payload() const noexcept {
        return payload_;
    }

    auto operator<=>(const StateSection&) const = default;

private:
    CanonicalPayload payload_;
};

struct GoalStateTag;
struct ValueStateTag;

using GoalState = StateSection<GoalStateTag>;
using ValueState = StateSection<ValueStateTag>;

// Weak source analogy: the author's state hash binds tensor and metadata
// content. This bounded sink and its binary preimage are additional C++
// storage infrastructure; they do not reproduce the Python JSON hash bytes.
class StateContentSink final {
public:
    // SWEGCA: src/swegca/mosaic_bounded_world_write.py@5901a5a:262-283
    template <class F>
        requires(!std::is_same_v<std::remove_cvref_t<F>, StateContentSink> &&
                 std::is_object_v<F> &&
                 std::is_invocable_v<F&,
                                     std::span<const std::byte>>)
    explicit StateContentSink(F& write) noexcept
        : target_(static_cast<const void*>(std::addressof(write))),
          call_(&invoke<F>) {}
    template <class F>
    StateContentSink(const F&&) = delete;

    // SWEGCA: src/swegca/mosaic_bounded_world_write.py@5901a5a:262-283
    void operator()(std::span<const std::byte> bytes) const { call_(target_, bytes); }

private:
    using Call = void (*)(const void*, std::span<const std::byte>);
    // SWEGCA: src/swegca/mosaic_bounded_world_write.py@5901a5a:262-283
    template <class F>
    static void invoke(const void* target, std::span<const std::byte> bytes) {
        auto& write = *static_cast<F*>(const_cast<void*>(target));
        write(bytes);
    }

    const void* target_;
    Call call_;
};

// The original bounded writer stores this current-write head under
// self_state["bounded_verification_write"]. It is state content: rollback
// restores the prior head exactly, and retraction checks receipt/revision.
struct BoundedWriteHead final {
    PolicyVersion policy_version;
    Digest256 receipt_id;
    std::uint64_t revision;
    RoleId target_role;
    // Preserve proposal order, including repeats, as in the original
    // receipt seed; do not sort this list during the guarded write.
    EvidenceReferences evidence_references;
};

class SelfState final {
public:
    // The caller's opaque payload cannot encode or impersonate write_head_.
    // The original reserved _WRITE_KEY is represented only by the typed head.
    // SWEGCA: src/swegca/mosaic_bounded_world_write.py@5901a5a:380-400
    explicit SelfState(CanonicalPayload payload)
        : payload_(std::move(payload)) {}
    // SWEGCA: src/swegca/mosaic_bounded_world_write.py@5901a5a:380-400
    SelfState(CanonicalPayload payload, BoundedWriteHead write_head)
        : payload_(std::move(payload)), write_head_(std::move(write_head)) {}

    // SWEGCA: src/swegca/mosaic_bounded_world_write.py@5901a5a:262-283
    [[nodiscard]] const CanonicalPayload& payload() const noexcept {
        return payload_;
    }
    // SWEGCA: src/swegca/mosaic_bounded_world_write.py@5901a5a:380-400
    [[nodiscard]] const std::optional<BoundedWriteHead>& write_head() const noexcept {
        return write_head_;
    }

private:
    CanonicalPayload payload_;
    std::optional<BoundedWriteHead> write_head_;
};

// The only persistent state type. Construction requires either Main's initial
// key or the guarded writer's successor key; producers receive StateSnapshot.
// The keys, registry and binary content digest are current C++ rules. The
// general state accepts any common batch dimension, as the source does. The
// guarded verification write alone requires batch one.
// SWEGCA: paper/swegca/ARCHITECTURE_SPEC.md@5901a5a:103-107
// SWEGCA: paper/swegca/ARCHITECTURE_SPEC.md@5901a5a:139-152
class CognitiveState final {
public:
    CognitiveState(InitialStateKey, OwnerId owner,
                   RoleRegistry roles, CognitiveTensor semantic,
                   CognitiveTensor executive, CognitiveTensor scratch,
                   StructuredWorldGraph world_graph,
                   EvidenceReferences evidence_references,
                   GoalState goals, ValueState values, SelfState self);

    CognitiveState(SuccessorStateKey, const CognitiveState& prior,
                   RoleRegistry roles,
                   CognitiveTensor semantic, CognitiveTensor executive,
                   CognitiveTensor scratch, StructuredWorldGraph world_graph,
                   EvidenceReferences evidence_references,
                   GoalState goals, ValueState values, SelfState self);

    CognitiveState(const CognitiveState&) = delete;
    CognitiveState& operator=(const CognitiveState&) = delete;
    CognitiveState(CognitiveState&&) = delete;
    CognitiveState& operator=(CognitiveState&&) = delete;
    ~CognitiveState() = default;

    // SWEGCA: src/swegca/mosaic_cognitive_kernel.py@5901a5a:220-254
    [[nodiscard]] const OwnerId& owner() const noexcept { return owner_; }
    // SWEGCA: src/swegca/mosaic_cognitive_kernel.py@5901a5a:220-254
    [[nodiscard]] const StateGeneration& generation() const noexcept {
        return generation_;
    }
    // Content only. The provisional StateGeneration wrapper is removed when
    // Main's journal-backed StateSnapshot can carry PublishedStateId.
    // SWEGCA: src/swegca/mosaic_bounded_world_write.py@5901a5a:262-283
    [[nodiscard]] const Digest256& content_digest() const noexcept {
        return generation_.digest();
    }
    // The same bytes, in the same order, that content_digest hashes. Each
    // borrowed span lives only through this call and must be copied or hashed
    // before the sink returns; no full-state buffer is materialized.
    void for_each_content_chunk(StateContentSink write) const;
    // SWEGCA: src/swegca/mosaic_cognitive_kernel.py@5901a5a:220-254
    [[nodiscard]] const RoleRegistry& roles() const noexcept { return roles_; }
    // SWEGCA: src/swegca/mosaic_cognitive_kernel.py@5901a5a:220-254
    [[nodiscard]] const CognitiveTensor& semantic() const noexcept {
        return semantic_;
    }
    // SWEGCA: src/swegca/mosaic_cognitive_kernel.py@5901a5a:220-254
    [[nodiscard]] const CognitiveTensor& executive() const noexcept {
        return executive_;
    }
    // SWEGCA: src/swegca/mosaic_cognitive_kernel.py@5901a5a:220-254
    [[nodiscard]] const CognitiveTensor& scratch() const noexcept {
        return scratch_;
    }
    // SWEGCA: src/swegca/mosaic_cognitive_kernel.py@5901a5a:220-254
    [[nodiscard]] const StructuredWorldGraph& world_graph() const noexcept {
        return world_graph_;
    }
    // SWEGCA: src/swegca/mosaic_cognitive_kernel.py@5901a5a:220-254
    [[nodiscard]] std::span<const ExperienceAddress> evidence_references()
        const noexcept { return evidence_references_; }
    // SWEGCA: src/swegca/mosaic_cognitive_kernel.py@5901a5a:220-254
    [[nodiscard]] const GoalState& goals() const noexcept { return goals_; }
    // SWEGCA: src/swegca/mosaic_cognitive_kernel.py@5901a5a:220-254
    [[nodiscard]] const ValueState& values() const noexcept { return values_; }
    // SWEGCA: src/swegca/mosaic_cognitive_kernel.py@5901a5a:220-254
    [[nodiscard]] const SelfState& self() const noexcept { return self_; }

private:
    void validate() const;
    [[nodiscard]] StateGeneration validated_generation(
        const CognitiveState* prior) const;

    OwnerId owner_;
    RoleRegistry roles_;
    CognitiveTensor semantic_;
    CognitiveTensor executive_;
    CognitiveTensor scratch_;
    StructuredWorldGraph world_graph_;
    EvidenceReferences evidence_references_;
    GoalState goals_;
    ValueState values_;
    SelfState self_;
    StateGeneration generation_;
};

class StateSnapshot final {
public:
    StateSnapshot(const StateSnapshot&) = default;
    StateSnapshot(StateSnapshot&&) noexcept = default;
    // Weak source analogy: the lease swap is native C++ infrastructure.
    // SWEGCA: paper/swegca/ARCHITECTURE_SPEC.md@5901a5a:103-107
    StateSnapshot& operator=(StateSnapshot other) noexcept;

    // SWEGCA: paper/swegca/ARCHITECTURE_SPEC.md@5901a5a:103-107
    [[nodiscard]] const CognitiveState& state() const;
    [[nodiscard]] const Digest256& content_digest() const {
        return state().content_digest();
    }

private:
    friend class MainOwner;

    // SWEGCA: paper/swegca/ARCHITECTURE_SPEC.md@5901a5a:103-107
    explicit StateSnapshot(std::shared_ptr<const CognitiveState> state,
                           std::shared_ptr<const void> main_lifetime);

    std::shared_ptr<const void> main_lifetime_;
    std::shared_ptr<const CognitiveState> state_;
};

}  // namespace swegca::architecture
