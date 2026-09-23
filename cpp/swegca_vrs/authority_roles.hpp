#pragma once

#include <cstdint>
#include <memory>

namespace swegca::vrs {

class StateSnapshot;
struct MainInitialState;
class AllocationContext;
class CognitiveState;
class BoundProposal;
class ExperienceJournal;
class MainAuthorityLedger;
struct GateOutcome;
struct ArbitrationOutcome;
struct BoundedWriteConfig;
struct BoundedWriteReceipt;
struct BoundedWriteResult;
namespace detail {
struct MainOwnerState;
struct MainStateWriterState;
}  // namespace detail

// Complete owner definition prevents a caller defining a substitute friend.
// The implementation owns the sole current state and its resource accounts.
// Rule: reconstruction board §2.1, §3A and §10.1; SWEGCA I01, I07, I10.
class MainOwner final {
public:
    MainOwner(MainInitialState initial, AllocationContext allocation);
    MainOwner(const MainOwner&) = delete;
    MainOwner& operator=(const MainOwner&) = delete;
    MainOwner(MainOwner&&) = delete;
    MainOwner& operator=(MainOwner&&) = delete;
    ~MainOwner();

    [[nodiscard]] StateSnapshot snapshot() const;

private:
    // Non-member storage receives no MainOwner friendship. In particular,
    // no incomplete nested type can be defined elsewhere to obtain it.
    std::shared_ptr<detail::MainOwnerState> state_;
};

// EvidenceGate is defined completely in evidence_gate.hpp, which authority.hpp
// includes at its end, so every translation unit that can name its issue key
// also sees its one definition.

// Main's guarded verification-slot writer (Stage 6), the only holder of the
// successor-state key and the cognitive-state-commit consume key. It is
// defined here, complete, for the same reason as MainOwner: every translation
// unit that can name those keys (cognitive_state.hpp and authority.hpp both
// include this header) sees this one definition, so no substitute friend can
// be defined. Its configuration, receipt and result types, and their use,
// are in main_state_writer.hpp; its operations in main_state_writer.cpp.
// It returns an immutable successor; replacing Main's current state with it
// is Main's step. Only MainOwner constructs it.
// SWEGCA: src/tinylm_slicer/mosaic_bounded_world_write.py@3bddcb7:379-541
class MainStateWriter final {
public:
    MainStateWriter(const MainStateWriter&) = delete;
    MainStateWriter& operator=(const MainStateWriter&) = delete;
    MainStateWriter(MainStateWriter&&) = delete;
    MainStateWriter& operator=(MainStateWriter&&) = delete;
    ~MainStateWriter();

    // The dedicated no-commit preview of one bound proposal under the writer's
    // bounded policy, at Main's published snapshot; the evidence gate judges
    // this preview.
    [[nodiscard]] ArbitrationOutcome preview(const StateSnapshot& snapshot,
                                             std::uint64_t current_step,
                                             const BoundProposal& bound) const;
    // Authorizes and, when `commit`, commits one verification-slot delta
    // against the state and publication of one Main snapshot. A commit
    // consumes the gate's capability; a dry run checks it only.
    [[nodiscard]] BoundedWriteResult write(const StateSnapshot& snapshot,
                                           const BoundProposal& bound, GateOutcome& gate,
                                           std::uint64_t current_step, bool commit);
    // Bit-exact rollback of the state a receipt produced.
    [[nodiscard]] std::shared_ptr<const CognitiveState> rollback(
        const CognitiveState& state, const BoundedWriteReceipt& receipt) const;
    // The receipt still owns the current verification-slot head.
    void validate_retraction(const CognitiveState& state,
                             const BoundedWriteReceipt& receipt) const;
    // Retracts the current write, keeping later unrelated changes.
    [[nodiscard]] std::shared_ptr<const CognitiveState> retract(
        const CognitiveState& state, const BoundedWriteReceipt& receipt) const;

private:
    friend class MainOwner;
    MainStateWriter(const ExperienceJournal& journal, MainAuthorityLedger& ledger,
                    const AllocationContext& memory, const BoundedWriteConfig& config);

    std::shared_ptr<detail::MainStateWriterState> state_;
};

// Complete, non-instantiable role definitions close friend-by-name passkey
// spoofing. Each role becomes constructible only when its architecture stage
// adds the complete Main-owned checks and operations to this definition.
#define SWEGCA_DECLARE_CLOSED_AUTHORITY_ROLE(Name) \
    class Name final {                           \
    public:                                      \
        Name() = delete;                         \
    }

SWEGCA_DECLARE_CLOSED_AUTHORITY_ROLE(SemanticMemoryGate);
SWEGCA_DECLARE_CLOSED_AUTHORITY_ROLE(SemanticMemoryWriter);
SWEGCA_DECLARE_CLOSED_AUTHORITY_ROLE(ExternalActionGate);
SWEGCA_DECLARE_CLOSED_AUTHORITY_ROLE(ExternalActionExecutor);
SWEGCA_DECLARE_CLOSED_AUTHORITY_ROLE(TrainingModelUpdateGate);
SWEGCA_DECLARE_CLOSED_AUTHORITY_ROLE(TrainingModelUpdateExecutor);
SWEGCA_DECLARE_CLOSED_AUTHORITY_ROLE(DistributionGate);
SWEGCA_DECLARE_CLOSED_AUTHORITY_ROLE(DistributionExecutor);
SWEGCA_DECLARE_CLOSED_AUTHORITY_ROLE(P3PromotionGate);
SWEGCA_DECLARE_CLOSED_AUTHORITY_ROLE(P3PromotionExecutor);

#undef SWEGCA_DECLARE_CLOSED_AUTHORITY_ROLE

}  // namespace swegca::vrs
