#pragma once

#include <cstdint>
#include <memory>

namespace swegca::architecture {

class StateSnapshot;
struct MainInitialState;
namespace detail { struct MainOwnerState; }

// Complete owner definition prevents a caller defining a substitute friend.
// The implementation owns the sole current state and its resource accounts.
// Rule: reconstruction board §2.1, §3A and §10.1; SWEGCA I01, I07, I10.
class MainOwner final {
public:
    MainOwner(MainInitialState initial, std::uint64_t memory_limit);
    MainOwner(const MainOwner&) = delete;
    MainOwner& operator=(const MainOwner&) = delete;
    MainOwner(MainOwner&&) = delete;
    MainOwner& operator=(MainOwner&&) = delete;
    ~MainOwner();

    [[nodiscard]] StateSnapshot snapshot() const;
    [[nodiscard]] std::uint64_t memory_requested() const noexcept;

private:
    // Non-member storage receives no MainOwner friendship. In particular,
    // no incomplete nested type can be defined elsewhere to obtain it.
    std::unique_ptr<detail::MainOwnerState> state_;
};

// EvidenceGate is defined completely in evidence_gate.hpp, which authority.hpp
// includes at its end, so every translation unit that can name its issue key
// also sees its one definition.

// Complete, non-instantiable role definitions close friend-by-name passkey
// spoofing. Each role becomes constructible only when its architecture stage
// adds the complete Main-owned checks and operations to this definition.
#define SWEGCA_DECLARE_CLOSED_AUTHORITY_ROLE(Name) \
    class Name final {                           \
    public:                                      \
        Name() = delete;                         \
    }

SWEGCA_DECLARE_CLOSED_AUTHORITY_ROLE(MainStateWriter);
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

}  // namespace swegca::architecture
