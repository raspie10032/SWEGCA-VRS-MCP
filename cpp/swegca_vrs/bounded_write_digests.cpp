#include "swegca_vrs/bounded_write_digests.hpp"

#include <array>
#include <charconv>
#include <stdexcept>
#include <system_error>

namespace swegca::vrs::detail {
namespace {

// Length-prefixed text so no two field sequences hash the same bytes.
// SWEGCA: src/tinylm_slicer/mosaic_bounded_world_write.py@3bddcb7:433-445
void hash_text(Sha256& hash, std::string_view text) {
    const auto length = static_cast<std::uint64_t>(text.size());
    std::array<std::byte, 8> prefix{};
    for (std::size_t at = 0; at < prefix.size(); ++at)
        prefix[at] = static_cast<std::byte>((length >> (8 * at)) & 0xff);
    hash.update(prefix);
    hash.update(text);
}

// SWEGCA: src/tinylm_slicer/mosaic_bounded_world_write.py@3bddcb7:433-445
void hash_u64(Sha256& hash, std::uint64_t value) {
    std::array<std::byte, 8> bytes{};
    for (std::size_t at = 0; at < bytes.size(); ++at)
        bytes[at] = static_cast<std::byte>((value >> (8 * at)) & 0xff);
    hash.update(bytes);
}

}  // namespace

// Lineage: direct — str(tensor.dtype) as slot_tensor_hash spells it.
// SWEGCA: src/tinylm_slicer/mosaic_cognitive_slot_memory.py@3bddcb7:22-28
std::string_view torch_dtype_name(ScalarType type) {
    switch (type) {
        case ScalarType::bfloat16: return "torch.bfloat16";
        case ScalarType::float16: return "torch.float16";
        case ScalarType::float32: return "torch.float32";
        case ScalarType::float64: return "torch.float64";
    }
    throw std::invalid_argument("bounded_write_scalar_type_invalid");
}

// The author's slot_tensor_hash of a [1, width] slot: SHA-256 over the dtype
// name, the JSON shape "[1,<width>]" and the little-endian element bytes.
// Lineage: direct — the same preimage bytes. For bfloat16 the author's
// tensor.numpy() raises (numpy has no bfloat16) although its receipt codec
// lists bfloat16; this native form hashes the stored bits instead.
// SWEGCA: src/tinylm_slicer/mosaic_cognitive_slot_memory.py@3bddcb7:22-28
SlotDigest::SlotDigest(ScalarType type, std::uint64_t width) {
    hash_.update(torch_dtype_name(type));
    std::array<char, 32> shape{};
    shape[0] = '[';
    shape[1] = '1';
    shape[2] = ',';
    const auto written = std::to_chars(shape.data() + 3, shape.data() + shape.size() - 1, width);
    if (written.ec != std::errc{})
        throw std::invalid_argument("bounded_write_slot_width_invalid");
    *written.ptr = ']';
    hash_.update(std::string_view(shape.data(),
                                  static_cast<std::size_t>(written.ptr + 1 - shape.data())));
}

// SWEGCA: src/tinylm_slicer/mosaic_cognitive_slot_memory.py@3bddcb7:22-28
void SlotDigest::update(std::span<const std::byte> bytes) { hash_.update(bytes); }

// SWEGCA: src/tinylm_slicer/mosaic_cognitive_slot_memory.py@3bddcb7:22-28
Digest256 SlotDigest::finish() { return Digest256(hash_.finish()); }

// SWEGCA: src/tinylm_slicer/mosaic_cognitive_slot_memory.py@3bddcb7:22-28
Digest256 slot_digest(ScalarType type, std::uint64_t width, std::span<const std::byte> bytes) {
    SlotDigest digest(type, width);
    digest.update(bytes);
    return digest.finish();
}

// Lineage: weak analogy — the author hashes a sorted-key JSON of the seed;
// this is a native length-prefixed preimage over the same fields.
// SWEGCA: src/tinylm_slicer/mosaic_bounded_world_write.py@3bddcb7:433-445
Digest256 bounded_write_receipt_id(const Digest256& before_state, const Digest256& delta,
                                   std::uint64_t revision, const EvidenceReferences& evidence,
                                   const ClaimRevision& claim, const Digest256& proposal_digest) {
    Sha256 hash;
    hash_text(hash, "swegca.bounded_write_receipt_id.v1");
    hash.update(before_state.bytes());
    hash.update(delta.bytes());
    hash_u64(hash, revision);
    hash_u64(hash, evidence.size());
    for (const auto& address : evidence) hash_text(hash, address.value());
    hash_text(hash, claim.claim().value());
    hash_u64(hash, claim.revision());
    hash.update(proposal_digest.bytes());
    return Digest256(hash.finish());
}

}  // namespace swegca::vrs::detail
