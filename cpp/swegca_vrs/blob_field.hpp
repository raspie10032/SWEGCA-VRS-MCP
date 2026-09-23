#pragma once

#include "swegca_vrs/core_sha256.hpp"
#include "swegca_vrs/journal_format.hpp"
#include "swegca_vrs/part_tree.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>

// One blob field's byte form, in one place, so the experience envelope and
// Main's state-section descriptors cannot drift apart: inline (mode 0) up to
// `part_tree::inline_top_bytes`, otherwise parted (mode 1) into
// `part_tree::part_bytes` parts whose depth and top count follow from the
// size. This layer knows neither record kinds nor addresses; each caller
// names and stages its own parts.
namespace swegca::vrs {

// Inline: mode, length, bytes. Parted: mode, size, digest, depth, top count,
// top digests.
inline constexpr std::size_t blob_field_inline_encoded_bytes = 1 + 4 + part_tree::inline_top_bytes;
inline constexpr std::size_t blob_field_parted_encoded_bytes =
    1 + 8 + 32 + 1 + 4 + part_tree::max_top_digests * digest256_width;

// One blob field as a record holds it; views the record's bytes. Inline
// (depth 0): `inline_bytes` holds all `size` bytes. Parted: the bytes are
// cut into parts of `part_tree::part_bytes` (the last may be shorter); the
// list of their digests is cut the same way while it is longer than an
// inline blob, level after level, until the top list fits: `depth` is the
// number of part levels (at most 3 for any u64 size) and `top_digests` the
// top list, 32 bytes per part, in order. Every level's count follows from
// `size`, so a blob has one form.
struct BlobField {
    std::uint64_t size = 0;
    DigestBytes digest{};  // SHA-256 of all `size` bytes
    std::uint8_t depth = 0;
    std::span<const std::byte> inline_bytes;
    std::span<const std::byte> top_digests;

    // SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:23-60
    [[nodiscard]] bool parted() const noexcept { return depth != 0; }
};

// Lineage: native mechanism — writes one blob field in its canonical inline form.
// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3fce8b5c605166d668924baa5d4a6c49dc0:567-568
inline void write_inline_blob_field(journal::ByteWriter& writer, std::span<const std::byte> bytes) {
    writer.u8(0);
    writer.bytes(bytes, part_tree::inline_top_bytes);
}

// Lineage: native mechanism — writes one blob field in its canonical parted form.
// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3fce8b5c605166d668924baa5d4a6c49dc0:567-568
inline void write_parted_blob_field(journal::ByteWriter& writer, std::uint64_t size,
                                    const DigestBytes& digest, std::uint8_t depth,
                                    std::span<const std::byte> top) {
    writer.u8(1);
    writer.u64(size);
    writer.digest(digest);
    writer.u8(depth);
    writer.u32(static_cast<std::uint32_t>(top.size() / digest256_width));
    writer.raw(top);
}

// Reads one blob field in its one form: inline up to the inline size,
// otherwise parted with exactly the depth and top count its size gives. Any
// other encoding throws std::invalid_argument(`invalid`), the caller's code.
// Lineage: native mechanism — reads one blob field only in its one canonical form, so any other encoding fails closed.
// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3fce8b5c605166d668924baa5d4a6c49dc0:567-568
inline BlobField read_blob_field(journal::ByteReader& reader, const char* invalid) {
    BlobField out;
    const auto mode = reader.u8();
    if (mode == 0) {
        out.inline_bytes = reader.bytes_view(part_tree::inline_top_bytes);
        out.size = out.inline_bytes.size();
        out.digest = Sha256::of(out.inline_bytes);
        return out;
    }
    if (mode != 1) throw std::invalid_argument(invalid);
    out.size = reader.u64();
    out.digest = reader.digest();
    out.depth = reader.u8();
    const auto count = reader.u32();
    if (out.size <= part_tree::inline_top_bytes) throw std::invalid_argument(invalid);
    const auto levels = part_tree::part_levels(out.size);
    if (out.depth != levels.depth || count != levels.counts[levels.depth - 1])
        throw std::invalid_argument(invalid);
    out.top_digests = reader.raw(static_cast<std::size_t>(count) * digest256_width);
    return out;
}

// The length of part `place` of `level` in parted data of `size` bytes: a
// full part, or what is left for the last one of its level. The caller keeps
// `level` and `place` within `levels`, which must be `part_levels(size)`.
// Lineage: native mechanism — the length each part must have from its place in the tree, so a part of any other size fails.
// SWEGCA: docs/SWEGCA_CPP_VRS_LAYER_PLAN.md@472d23225c973fa0a33581afd6bd9026df6fc98a:522-523
inline std::uint64_t blob_part_length(const part_tree::PartLevels& levels, std::uint64_t size,
                                      std::uint8_t level, std::uint64_t place) noexcept {
    if (level == 0)
        return std::min<std::uint64_t>(part_tree::part_bytes, size - place * part_tree::part_bytes);
    return std::min<std::uint64_t>(part_tree::digests_per_part,
                                   levels.counts[level - 1] - place * part_tree::digests_per_part) *
           digest256_width;
}

}  // namespace swegca::vrs
