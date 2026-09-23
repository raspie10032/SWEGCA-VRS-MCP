#pragma once

#include "swegca_vrs/core_kernel.hpp"

#include "swegca_architecture/evidence_rules.hpp"

// The core evidence policy names the VRS uses (evidence_rules.hpp).
// SWEGCA: user@2026-09-22:62
namespace swegca::vrs {

using architecture::evidence_policy_digest;
using architecture::EvidencePolicy;
using architecture::make_evidence_rules;

}  // namespace swegca::vrs
