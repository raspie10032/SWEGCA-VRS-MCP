#pragma once

#include "swegca_architecture/allocation.hpp"
#include "swegca_architecture/digest_bytes.hpp"
#include "swegca_architecture/evidence_kernel.hpp"
#include "swegca_architecture/evidence_rules.hpp"
#include "swegca_architecture/sha256.hpp"
#include "swegca_architecture/strong_types.hpp"

// Explicitly bridge the existing verifier value types into the VRS layer.
// The VRS-only identities in strong_types are moved out in a separate change.
namespace swegca::vrs {
using architecture::AllocationAdapter;
using architecture::AllocationContext;
using architecture::AllocationRefused;
using architecture::ClaimId;
using architecture::ClaimIdTag;
using architecture::ClaimRevision;
using architecture::Digest256;
using architecture::DigestBytes;
using architecture::EntityId;
using architecture::EntityKind;
using architecture::EvidencePolicy;
using architecture::ExperienceAddress;
using architecture::ExperienceAddressTag;
using architecture::NoAuthority;
using architecture::OwnerId;
using architecture::PolicyVersion;
using architecture::ProducerId;
using architecture::ProducerIdTag;
using architecture::RelationPredicate;
using architecture::RoleId;
using architecture::Sha256;
using architecture::StateGeneration;
using architecture::TextIdentity;
using architecture::TransactionId;
using architecture::TransactionIdTag;
using architecture::digest256_width;
using architecture::evidence_policy_digest;
using architecture::make_evidence_rules;
using architecture::zero_digest_bytes;
}

namespace swegca::vrs::detail {
using architecture::detail::identity_text_max_bytes;
using architecture::detail::is_identity_text;
using architecture::detail::is_strict_utf8;
using architecture::detail::require_identity_text;
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
