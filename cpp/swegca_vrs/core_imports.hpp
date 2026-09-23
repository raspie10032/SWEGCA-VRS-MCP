#pragma once

#include "swegca_architecture/allocation.hpp"
#include "swegca_architecture/digest_bytes.hpp"
#include "swegca_architecture/evidence_kernel.hpp"
#include "swegca_architecture/evidence_rules.hpp"
#include "swegca_architecture/sha256.hpp"
#include "swegca_vrs/identity_types.hpp"

// Explicitly bridge verifier values into the VRS layer.
namespace swegca::vrs {
using architecture::AllocationAdapter;
using architecture::AllocationContext;
using architecture::AllocationRefused;
using architecture::Digest256;
using architecture::DigestBytes;
using architecture::EvidencePolicy;
using architecture::Sha256;
using architecture::digest256_width;
using architecture::evidence_policy_digest;
using architecture::make_evidence_rules;
using architecture::zero_digest_bytes;
}

namespace swegca::vrs::kernel {
using architecture::kernel::Digest;
using architecture::kernel::EvidenceJudgment;
using architecture::kernel::EvidenceRules;
using architecture::kernel::EvidenceStatus;
using architecture::kernel::EvidenceTally;
using architecture::kernel::finite_unit;
using architecture::kernel::judge_evidence;
}
