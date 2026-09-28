#pragma once
#include "vrs/evidence_executor.hpp"
#include <memory>
#include <string>
namespace swegca::vrs {
class GpuEvidence final:public EvidenceExecutor {
 struct Impl;std::unique_ptr<Impl> impl_;
public:
 explicit GpuEvidence(const std::string& progress_directory);
 ~GpuEvidence();
 architecture::kernel::EvidenceJudgment judge(const architecture::kernel::EvidenceRules&,const architecture::kernel::EvidenceTally&) override;
};
}
