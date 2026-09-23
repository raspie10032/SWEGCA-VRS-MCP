#pragma once

namespace swegca::architecture {

// Complete, non-instantiable role definitions close friend-by-name passkey
// spoofing. Each role becomes constructible only when its architecture stage
// adds the complete Main-owned checks and operations to this definition.
#define SWEGCA_DECLARE_CLOSED_AUTHORITY_ROLE(Name) \
    class Name final {                           \
    public:                                      \
        Name() = delete;                         \
    }

SWEGCA_DECLARE_CLOSED_AUTHORITY_ROLE(MainOwner);
SWEGCA_DECLARE_CLOSED_AUTHORITY_ROLE(EvidenceGate);
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
