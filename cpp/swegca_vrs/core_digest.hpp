#pragma once

#include "swegca_architecture/digest_bytes.hpp"

// Verifier values are imported into VRS by the relevant core_* header;
// VRS identities live in identity_types.hpp. The core never includes VRS.
// Qualified exceptions: resource_budget.* names
// architecture::AllocationResource and AllocationRefused, and
// authority_roles.hpp forward-declares architecture::AllocationContext.
// This header: digest_bytes.hpp.
// SWEGCA: user@2026-09-22:62
namespace swegca::vrs {

using architecture::digest256_width;
using architecture::DigestBytes;
using architecture::zero_digest_bytes;

}  // namespace swegca::vrs
