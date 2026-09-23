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
#include <utility>
#include <vector>

namespace swegca::architecture {

class InitialStateKey final {
public:
    InitialStateKey(const InitialStateKey&) = delete;
    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:222-230
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
    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:222-230
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
    CanonicalPayload(const AllocationContext& account,
                     std::span<const std::byte> bytes);

    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:83-93
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
        return bytes_;
    }

    auto operator<=>(const CanonicalPayload&) const = default;

private:
    std::vector<std::byte, AllocationAdapter<std::byte>> bytes_;
};

struct WorldEntity final {
    EntityId id;
    EntityKind kind;
    CanonicalPayload attributes;

    auto operator<=>(const WorldEntity&) const = default;
};

struct WorldRelation final {
    RelationId id;
    RelationKind kind;
    EntityId source;
    EntityId target;
    CanonicalPayload attributes;

    auto operator<=>(const WorldRelation&) const = default;
};

// Borrowed construction inputs; graph-owned identities, payloads and arrays
// are copied only through Main's allocator.
struct WorldEntityInput final {
    std::string_view id;
    std::string_view kind;
    std::span<const std::byte> attributes;
};

struct WorldRelationInput final {
    std::string_view id;
    std::string_view kind;
    std::string_view source;
    std::string_view target;
    std::span<const std::byte> attributes;
};

class StructuredWorldGraph final {
public:
    StructuredWorldGraph(const AllocationContext& account,
                         std::span<const WorldEntityInput> entities,
                         std::span<const WorldRelationInput> relations);

    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:83-93
    [[nodiscard]] std::span<const WorldEntity> entities() const noexcept {
        return entities_;
    }
    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:83-93
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
    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:83-93
    explicit StateSection(CanonicalPayload payload)
        : payload_(std::move(payload)) {}

    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:83-93
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

using EvidenceReferences =
    std::vector<ExperienceAddress, AllocationAdapter<ExperienceAddress>>;

// The original bounded writer stores this current-write head under
// self_state["bounded_verification_write"]. It is state content: rollback
// restores the prior head exactly, and retraction checks receipt/revision.
struct BoundedWriteHead final {
    Digest256 receipt_id;
    std::uint64_t revision;
    RoleId target_role;
    EvidenceReferences evidence_references;
};

class SelfState final {
public:
    explicit SelfState(CanonicalPayload payload)
        : payload_(std::move(payload)) {}
    SelfState(CanonicalPayload payload, BoundedWriteHead write_head)
        : payload_(std::move(payload)), write_head_(std::move(write_head)) {}

    [[nodiscard]] const CanonicalPayload& payload() const noexcept {
        return payload_;
    }
    [[nodiscard]] const std::optional<BoundedWriteHead>& write_head() const noexcept {
        return write_head_;
    }

private:
    CanonicalPayload payload_;
    std::optional<BoundedWriteHead> write_head_;
};

// The only persistent state type. Construction requires either Main's initial
// key or the guarded writer's successor key; producers receive StateSnapshot.
// Rule: Single-World state, reconstruction board@7c0b62f:83-93,199-200,222-230.
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

    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:83-93
    [[nodiscard]] const OwnerId& owner() const noexcept { return owner_; }
    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:83-93
    [[nodiscard]] const StateGeneration& generation() const noexcept {
        return generation_;
    }
    // Content only. The provisional StateGeneration wrapper is removed when
    // Main's journal-backed StateSnapshot can carry PublishedStateId.
    [[nodiscard]] const Digest256& content_digest() const noexcept {
        return generation_.digest();
    }
    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:83-93
    [[nodiscard]] const RoleRegistry& roles() const noexcept { return roles_; }
    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:83-93
    [[nodiscard]] const CognitiveTensor& semantic() const noexcept {
        return semantic_;
    }
    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:83-93
    [[nodiscard]] const CognitiveTensor& executive() const noexcept {
        return executive_;
    }
    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:83-93
    [[nodiscard]] const CognitiveTensor& scratch() const noexcept {
        return scratch_;
    }
    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:83-93
    [[nodiscard]] const StructuredWorldGraph& world_graph() const noexcept {
        return world_graph_;
    }
    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:83-93
    [[nodiscard]] std::span<const ExperienceAddress> evidence_references()
        const noexcept { return evidence_references_; }
    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:83-93
    [[nodiscard]] const GoalState& goals() const noexcept { return goals_; }
    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:83-93
    [[nodiscard]] const ValueState& values() const noexcept { return values_; }
    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:83-93
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
    // SWEGCA: user@2026-09-23:1
    StateSnapshot& operator=(StateSnapshot other) noexcept;

    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:199-200
    [[nodiscard]] const CognitiveState& state() const;
    [[nodiscard]] const Digest256& content_digest() const {
        return state().content_digest();
    }

private:
    friend class MainOwner;

    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:222-230
    explicit StateSnapshot(std::shared_ptr<const CognitiveState> state,
                           std::shared_ptr<const void> main_lifetime);

    std::shared_ptr<const void> main_lifetime_;
    std::shared_ptr<const CognitiveState> state_;
};

}  // namespace swegca::architecture
