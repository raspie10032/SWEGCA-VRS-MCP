#pragma once

#include "swegca_vrs/cognitive_state.hpp"
#include "swegca_vrs/core_sha256.hpp"
#include "swegca_vrs/identity_types.hpp"
#include "swegca_vrs/native_tensor.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

// The two digests a bounded World write and its receipt codec share: the
// author's slot_tensor_hash of a [1, width] slot and the receipt id over the
// author's seed. Both are the preimages MainStateWriter has always hashed,
// moved here unchanged so the codec can check a decoded receipt with them.
namespace swegca::vrs::detail {

// str(tensor.dtype) as slot_tensor_hash spells it.
// SWEGCA: src/tinylm_slicer/mosaic_cognitive_slot_memory.py@3bddcb7:22-28
[[nodiscard]] std::string_view torch_dtype_name(ScalarType type);

// slot_tensor_hash fed as its bytes arrive: the dtype name and the shape
// text "[1,<width>]" at construction, then the little-endian element bytes.
// A slot read in pieces hashes the same as the whole slot.
class SlotDigest final {
public:
    // SWEGCA: src/tinylm_slicer/mosaic_cognitive_slot_memory.py@3bddcb7:22-28
    SlotDigest(ScalarType type, std::uint64_t width);
    // SWEGCA: src/tinylm_slicer/mosaic_cognitive_slot_memory.py@3bddcb7:22-28
    void update(std::span<const std::byte> bytes);
    // SWEGCA: src/tinylm_slicer/mosaic_cognitive_slot_memory.py@3bddcb7:22-28
    [[nodiscard]] Digest256 finish();

private:
    Sha256 hash_;
};

// The whole slot at once.
// SWEGCA: src/tinylm_slicer/mosaic_cognitive_slot_memory.py@3bddcb7:22-28
[[nodiscard]] Digest256 slot_digest(ScalarType type, std::uint64_t width,
                                    std::span<const std::byte> bytes);

// The receipt id over the author's seed: before-state hash, delta hash,
// revision, evidence references in proposal order, the claim (the author's
// hypothesis_id) and the proposal digest (its proposal_binding_digest).
// SWEGCA: src/tinylm_slicer/mosaic_bounded_world_write.py@3bddcb7:433-445
[[nodiscard]] Digest256 bounded_write_receipt_id(const Digest256& before_state, const Digest256& delta,
                                                 std::uint64_t revision, const EvidenceReferences& evidence,
                                                 const ClaimRevision& claim, const Digest256& proposal_digest);

}  // namespace swegca::vrs::detail
