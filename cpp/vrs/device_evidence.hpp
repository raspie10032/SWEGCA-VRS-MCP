#pragma once
#include "swegca_architecture/evidence_kernel.hpp"
#include "swegca_architecture/association_kernel.hpp"
#include <memory>
#include <span>
namespace swegca::vrs {
// One execution endpoint, not one logical SWEGCA core. A call carries many
// independent core operations. WorkPipeline allocates ready work to endpoints.
class DeviceEvidence final {
public:
 explicit DeviceEvidence(int cuda_device);
 ~DeviceEvidence();
 static int available();
 std::size_t capacity()const;
 void associate(std::span<const architecture::kernel::AssociationEvidence>,std::span<architecture::kernel::AssociationJudgment>);
 void judge(const architecture::kernel::EvidenceRules&,
   std::span<const architecture::kernel::EvidenceTally>,
   std::span<architecture::kernel::EvidenceJudgment>);
private:
 struct Impl;std::unique_ptr<Impl> impl_;
};
void cpu_evidence_batch(const architecture::kernel::EvidenceRules&,
 std::span<const architecture::kernel::EvidenceTally>,
 std::span<architecture::kernel::EvidenceJudgment>);
}
