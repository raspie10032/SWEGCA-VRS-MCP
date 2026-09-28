#pragma once
#include "swegca_architecture/evidence_kernel.hpp"
namespace swegca::vrs {
class EvidenceExecutor { public:virtual ~EvidenceExecutor()=default;virtual architecture::kernel::EvidenceJudgment judge(const architecture::kernel::EvidenceRules&,const architecture::kernel::EvidenceTally&)=0; };
// Selected only by the serialized VRS owner for its current worker scope.
// The scalar SWEGCA core remains pure and has no device/routing state.
inline thread_local EvidenceExecutor* current_evidence_executor=nullptr;
class EvidenceExecutorScope {EvidenceExecutor* previous_;public:explicit EvidenceExecutorScope(EvidenceExecutor* value):previous_(current_evidence_executor){current_evidence_executor=value;}~EvidenceExecutorScope(){current_evidence_executor=previous_;}};
}
