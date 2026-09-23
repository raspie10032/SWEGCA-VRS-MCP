#pragma once

#include "swegca_vrs/main_state_writer.hpp"

#include <span>
#include <variant>
#include <vector>

// A memory candidate may cite an original experience and the write receipt
// that makes semantic promotion eligible. These are different identities:
// Recall/Re-evidence can replay an ExperienceAddress, while a write-receipt
// link names a Main-held receipt. Neither reference grants authority.
// SWEGCA: src/tinylm_slicer/mosaic_world_memory_transaction.py@3bddcb7:234-239
namespace swegca::vrs {

struct WriteReceiptLink final {
    Digest256 receipt_id;
};

using MemoryCandidateReference = std::variant<ExperienceAddress, WriteReceiptLink>;
using MemoryCandidateReferences =
    std::vector<MemoryCandidateReference, AllocationAdapter<MemoryCandidateReference>>;

// The author's required_refs.issubset(candidate.evidence_refs), represented
// with distinct native identities. Order and repeats in either sequence are
// left intact; inclusion ignores multiplicity and allows extra candidate
// references. An empty candidate has no valid MemoryCandidate source form.
// This predicate does not authenticate the receipt, check Main's current
// state/journal heads, or issue semantic-promotion authority.
// It takes one receipt so its evidence references and id cannot be paired
// from different receipts. The allocation-free scan takes
// O(receipt.evidence_references.size() * candidate.size()) time.
// Lineage: direct — the source's set-inclusion predicate; native mechanism —
// the two reference kinds cannot impersonate one another.
// SWEGCA: src/tinylm_slicer/mosaic_memory_promotion.py@3bddcb7:37-71
// SWEGCA: src/tinylm_slicer/mosaic_world_memory_transaction.py@3bddcb7:234-239
[[nodiscard]] inline bool memory_candidate_covers_write_receipt(
    std::span<const MemoryCandidateReference> candidate,
    const BoundedWriteReceipt& receipt) noexcept {
    if (candidate.empty()) return false;

    bool linked = false;
    for (const auto& reference : candidate) {
        const auto* link = std::get_if<WriteReceiptLink>(&reference);
        if (link != nullptr && link->receipt_id == receipt.receipt_id) {
            linked = true;
            break;
        }
    }
    if (!linked) return false;

    for (const auto& required : receipt.evidence_references) {
        bool found = false;
        for (const auto& reference : candidate) {
            const auto* address = std::get_if<ExperienceAddress>(&reference);
            if (address != nullptr && address->value() == required.value()) {
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    return true;
}

}  // namespace swegca::vrs
