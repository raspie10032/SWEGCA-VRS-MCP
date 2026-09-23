#pragma once

#include "swegca_architecture/authority_roles.hpp"
#include "swegca_architecture/native_tensor.hpp"
#include "swegca_architecture/role_registry.hpp"
#include "swegca_architecture/strong_types.hpp"

#include <compare>
#include <cstddef>
#include <memory>
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
    CanonicalPayload(const MemoryLedger::Account& account,
                     std::span<const std::byte> bytes);

    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:83-93
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
        return bytes_;
    }

    auto operator<=>(const CanonicalPayload&) const = default;

private:
    std::vector<std::byte, MemoryLedger::Allocator<std::byte>> bytes_;
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

// Borrowed construction inputs; graph-owned payloads and arrays are copied
// only after Main's allocator has reserved their bytes.
struct WorldEntityInput final {
    EntityId id;
    EntityKind kind;
    std::span<const std::byte> attributes;
};

struct WorldRelationInput final {
    RelationId id;
    RelationKind kind;
    EntityId source;
    EntityId target;
    std::span<const std::byte> attributes;
};

class StructuredWorldGraph final {
public:
    StructuredWorldGraph(const MemoryLedger::Account& account,
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
    std::vector<WorldEntity, MemoryLedger::Allocator<WorldEntity>> entities_;
    std::vector<WorldRelation, MemoryLedger::Allocator<WorldRelation>> relations_;
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
struct SelfStateTag;

using GoalState = StateSection<GoalStateTag>;
using ValueState = StateSection<ValueStateTag>;
using SelfState = StateSection<SelfStateTag>;

// The only persistent state type. Construction requires either Main's initial
// key or the guarded writer's successor key; producers receive StateSnapshot.
// Rule: Single-World state, reconstruction board@7c0b62f:83-93,199-200,222-230.
class CognitiveState final {
public:
    CognitiveState(InitialStateKey, OwnerId owner,
                   RoleRegistry roles, CognitiveTensor semantic,
                   CognitiveTensor executive, CognitiveTensor scratch,
                   StructuredWorldGraph world_graph,
                   std::vector<ExperienceAddress> evidence_references,
                   GoalState goals, ValueState values, SelfState self);

    CognitiveState(SuccessorStateKey, const CognitiveState& prior,
                   RoleRegistry roles,
                   CognitiveTensor semantic, CognitiveTensor executive,
                   CognitiveTensor scratch, StructuredWorldGraph world_graph,
                   std::vector<ExperienceAddress> evidence_references,
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
    [[nodiscard]] const std::vector<ExperienceAddress>& evidence_references()
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
    std::vector<ExperienceAddress> evidence_references_;
    GoalState goals_;
    ValueState values_;
    SelfState self_;
    StateGeneration generation_;
};

class StateSnapshot final {
public:
    StateSnapshot(const StateSnapshot&) = default;
    StateSnapshot(StateSnapshot&&) noexcept = default;
    StateSnapshot& operator=(const StateSnapshot&) = default;
    StateSnapshot& operator=(StateSnapshot&&) noexcept = default;

    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:199-200
    [[nodiscard]] const CognitiveState& state() const noexcept { return *state_; }

private:
    friend class MainOwner;

    // SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@7c0b62f:222-230
    explicit StateSnapshot(std::shared_ptr<const CognitiveState> state);

    std::shared_ptr<const CognitiveState> state_;
};

}  // namespace swegca::architecture
