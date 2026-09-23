#pragma once

#include "swegca_vrs/allocation.hpp"
#include "swegca_vrs/blob_field.hpp"
#include "swegca_vrs/cognitive_state.hpp"
#include "swegca_vrs/part_tree.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <type_traits>

// Main's cognitive-state record codec, its storage half: the canonical v4
// content stream of one CognitiveState, cut at its eight section boundaries,
// one bounded descriptor per section, and one fixed-size root over the eight.
// A plain section (prefix, entities, relations, evidence, final fields) is
// one shared blob field (blob_field.hpp), the experience envelope's rule:
// inline up to 2 MiB, otherwise parted into fixed 8 MiB parts. A tensor
// section is always parted, an empty one included, with no inline form: its
// descriptor keeps the fixed header, and its `tensor_chunks` marker starts
// the parted bytes, so its level-0 parts are exactly the tensor's stored
// chunks. A tensor descriptor carries no whole-data digest; the content
// digest covers the data.
//
// The graph is a pure function of the canonical bytes: fixed offsets, one
// inline rule, levels from sizes, one encoding per record. Identical content
// therefore gives the same parts, descriptors and root on every retry or
// rollback, and a writer that finds one of them already published can verify
// and reuse it.
//
// This layer returns bytes and digests only. It stages nothing, publishes
// nothing, resolves nothing, sets no head and grants no authority. Main's
// storage adapter stages each part address once per generation, verifies an
// already-published part (kind, full payload, length) before reusing it, and
// alone uses the Main-only state kinds (JournalStore::stage_state_records).
// A kind-7 publication body, Main's marker and cold recovery are outside this
// layer. Recovery is not implemented: a decoded section is not a recovered
// state until the rebuilt stream hashes to the root's content digest and
// passes the canonical CognitiveState validation, under Main.
namespace swegca::vrs {

inline constexpr std::string_view state_part_address_prefix = "state-part:";
inline constexpr std::string_view state_root_address_prefix = "state-root:";
inline constexpr std::size_t state_part_address_bytes =
    state_part_address_prefix.size() + 2 * digest256_width;
inline constexpr std::size_t state_root_address_bytes =
    state_root_address_prefix.size() + 2 * digest256_width;

// Sections in stream order: prefix, semantic, executive, scratch tensors,
// entities, relations, evidence, final fields.
inline constexpr std::size_t state_section_count = 8;
// A tensor section's header: partition, scalar type and byte order (u8
// each), batches, slots, width and byte count (u64 each).
inline constexpr std::size_t state_tensor_header_bytes = 3 + 4 * 8;
// Root: magic, version, content digest, stream length, then per section its
// length and its descriptor's digest.
inline constexpr std::size_t state_root_payload_bytes =
    4 + 2 + digest256_width + 8 + state_section_count * (8 + digest256_width);
// Tensor descriptor: form, data length, header, depth, top count, top
// digests; the top list is at most one inline top list of the part tree. A
// plain descriptor is one blob field, never longer.
inline constexpr std::size_t state_descriptor_max_bytes =
    1 + 8 + state_tensor_header_bytes + 1 + 4 + part_tree::inline_top_bytes;
static_assert(blob_field_inline_encoded_bytes <= state_descriptor_max_bytes &&
                  blob_field_parted_encoded_bytes <= state_descriptor_max_bytes,
              "a plain descriptor fits the descriptor bound");
static_assert(state_descriptor_max_bytes <= part_tree::part_bytes,
              "a descriptor is always one part");

enum class StateSectionForm : std::uint8_t {
    // One blob field; its first byte is the blob mode, 0 or 1.
    plain = 1,
    // The first byte of a tensor descriptor, distinct from both blob modes.
    tensor = 2,
};

// Lineage: native mechanism — which of the canonical stream's eight sections
// carry a tensor header; the stream order is the emitter's.
// SWEGCA: src/swegca/mosaic_bounded_world_write.py@5901a5a:262-283
[[nodiscard]] constexpr StateSectionForm state_section_form(std::size_t section) noexcept {
    return section >= 1 && section <= 3 ? StateSectionForm::tensor : StateSectionForm::plain;
}

struct StateSectionEntry {
    std::uint64_t section_bytes = 0;  // header plus data for a tensor section
    DigestBytes descriptor{};         // SHA-256 of the descriptor part
};

// A kind-6 root's fields, addressed `state-root:<hex content_digest>`.
struct StateRootFields {
    DigestBytes content_digest{};
    std::uint64_t stream_bytes = 0;
    std::array<StateSectionEntry, state_section_count> sections{};
};

// A decoded descriptor; its spans view the descriptor's bytes.
struct StateSectionDescriptor {
    StateSectionForm form = StateSectionForm::plain;
    std::span<const std::byte> tensor_header;  // empty for a plain section
    // A plain section: its bytes as one blob field, inline or parted. A
    // tensor section: its data's size, depth and top digests; always parted,
    // never inline, and `digest` stays zero.
    BlobField data;
};

// Receives each part the splitter cuts, in stream order: per section its
// level-0 parts, its upper digest-list parts bottom-up, then its descriptor
// (an inline plain section emits its descriptor only). The span lives only
// through the call. The same digest may come more than once (equal parts);
// the caller stages each address once.
class StatePartSink final {
public:
    // Lineage: native mechanism — a borrowed callback, as StateContentSink.
    // SWEGCA: docs/SWEGCA_CPP_VRS_LAYER_PLAN.md@472d23225c973fa0a33581afd6bd9026df6fc98a:64
    template <class F>
        requires(!std::is_same_v<std::remove_cvref_t<F>, StatePartSink> &&
                 std::is_object_v<F> &&
                 std::is_invocable_v<F&, const DigestBytes&, std::span<const std::byte>>)
    explicit StatePartSink(F& emit) noexcept
        : target_(static_cast<const void*>(std::addressof(emit))), call_(&invoke<F>) {}
    template <class F>
    StatePartSink(const F&&) = delete;

    // SWEGCA: docs/SWEGCA_CPP_VRS_LAYER_PLAN.md@472d23225c973fa0a33581afd6bd9026df6fc98a:64
    void operator()(const DigestBytes& digest, std::span<const std::byte> bytes) const {
        call_(target_, digest, bytes);
    }

private:
    using Call = void (*)(const void*, const DigestBytes&, std::span<const std::byte>);
    // SWEGCA: docs/SWEGCA_CPP_VRS_LAYER_PLAN.md@472d23225c973fa0a33581afd6bd9026df6fc98a:64
    template <class F>
    static void invoke(const void* target, const DigestBytes& digest,
                       std::span<const std::byte> bytes) {
        auto& emit = *static_cast<F*>(const_cast<void*>(target));
        emit(digest, bytes);
    }

    const void* target_;
    Call call_;
};

// `state-part:<hex digest>` and `state-root:<hex content digest>`.
[[nodiscard]] std::array<char, state_part_address_bytes> state_part_address(
    const DigestBytes& digest) noexcept;
[[nodiscard]] std::array<char, state_root_address_bytes> state_root_address(
    const DigestBytes& content_digest) noexcept;

// Cuts `state`'s canonical stream into parts and descriptors, hands each to
// `emit`, and returns the root fields. Holds one part buffer, the level-0
// digest list and upper levels of the open section, and one descriptor, all
// charged to `memory`; never a copy of a tensor. Throws on a stream that
// breaks the section order or header form, and passes on whatever `emit`
// throws.
[[nodiscard]] StateRootFields split_state_content(const CognitiveState& state,
                                                  const AllocationContext& memory,
                                                  StatePartSink emit);

// The root's one encoding; the fields must be consistent (tensor sections at
// least a header long, lengths summing to `stream_bytes` without overflow).
[[nodiscard]] std::array<std::byte, state_root_payload_bytes> encode_state_root(
    const StateRootFields& root);
// Decodes a root payload in its one form, or throws.
[[nodiscard]] StateRootFields decode_state_root(std::span<const std::byte> payload);

// Decodes descriptor `payload` of section `section`, which the root gives
// `section_bytes`, in its one form, or throws. Every length and level count
// is checked here, before any part is read.
[[nodiscard]] StateSectionDescriptor decode_state_descriptor(
    std::span<const std::byte> payload, std::size_t section, std::uint64_t section_bytes);

// The exact length of part `index` of `level` for `data_bytes` of parted
// data (a tensor's data, or a parted plain section): a full part or what is
// left for the last one of its level, the shared blob rule. Throws for a
// level or index the size does not have.
[[nodiscard]] std::uint64_t state_part_length(std::uint64_t data_bytes, std::uint8_t level,
                                              std::uint64_t index);

}  // namespace swegca::vrs
