#pragma once

#include "swegca_vrs/core_digest.hpp"

#include "swegca_architecture/evidence_kernel.hpp"

// The core kernel names the VRS kernels use. Kept apart from core.hpp so a
// VRS kernel header stays free of allocation, string and exception headers.
// SWEGCA: user@2026-09-22:62
namespace swegca::vrs {

namespace kernel {
using architecture::kernel::Digest;
using architecture::kernel::EvidenceJudgment;
using architecture::kernel::EvidenceRules;
using architecture::kernel::EvidenceStatus;
using architecture::kernel::EvidenceTally;
using architecture::kernel::finite_unit;
using architecture::kernel::judge_evidence;
}  // namespace kernel

}  // namespace swegca::vrs
