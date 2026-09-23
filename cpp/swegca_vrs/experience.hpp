#pragma once

#include "swegca_vrs/core_digest.hpp"
#include "swegca_vrs/allocation.hpp"

#include "swegca_architecture/digest_bytes.hpp"
#include "swegca_vrs/journal_format.hpp"
#include "swegca_vrs/journal_store.hpp"
#include "swegca_vrs/allocation.hpp"
#include "swegca_vrs/identity_types.hpp"
#include "swegca_vrs/part_tree.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

// Main-owned original experience (board §3B, §4, §5): complete observations
// appended unfiltered to Main's journal under stable digest-bound addresses,
// decoded back exactly, found through the typed index views (cue, source,
// content digest, lineage, successor for validity and supersession,
// namespace, resource, transaction), and
// selected for cognition through the cue view with a receipt whose
// authority is statically none. Nothing here grants authority: a record
// documents, a receipt audits, and neither converts to a capability. Main
// publishes what is staged here; only Main does.
// Rules: board @cefdc3f §3B :116-126, §4 :200-212, §5 :282-286, §9 :592-595;
// L3 mosaic_unrestricted_experience.py@5901a5a (artifacts 23-155, selection
// receipts 158-290, hot index 293-339, discovery/index/selection 342-537);
// mosaic_external_memory.py:15-42,164-221 (resource, content, namespace
// views) and mosaic_versioned_memory.py:36-44,139-202 (validity and
// supersession) at the same revision.
namespace swegca::vrs {

class MainOwner;

// Journal record kinds this module owns: an original experience, and one
// derived from earlier experience (it names what it was derived from). Only
// these records may carry lowercase index kinds (journal_format.hpp).
inline constexpr std::uint16_t original_experience_kind = journal::original_experience_record_kind;
inline constexpr std::uint16_t derived_experience_kind = journal::derived_experience_record_kind;
inline constexpr std::string_view experience_address_prefix = "experience:";
inline constexpr std::size_t experience_address_bytes = experience_address_prefix.size() + 2 * digest256_width;
// An experience's bytes too large for its record are kept as parts: records
// of their own kind addressed by the SHA-256 of their bytes (so equal parts
// are one record), each `experience_part_bytes` long except the last.
inline constexpr std::uint16_t experience_part_kind = journal::experience_part_record_kind;
inline constexpr std::string_view experience_part_address_prefix = "experience-part:";
inline constexpr std::size_t experience_part_address_bytes =
    experience_part_address_prefix.size() + 2 * digest256_width;
inline constexpr std::size_t experience_part_bytes = part_tree::part_bytes;
// A blob (raw bytes, structured bytes, root sources) up to this size is kept
// in the experience record itself; a larger one is kept as parts.
inline constexpr std::size_t experience_inline_blob_bytes = part_tree::inline_top_bytes;
// A caller's cues for one memory are kept beside it, never in it: a record
// of their own kind addressed under the memory's address (the memory's
// address, this infix, and the digest of the binding), so a cue lookup that
// finds it names the memory by its first `experience_address_bytes`.
inline constexpr std::uint16_t cue_binding_kind = journal::cue_binding_record_kind;
inline constexpr std::string_view cue_binding_infix = "/cue-binding:";
inline constexpr std::size_t cue_binding_address_bytes =
    experience_address_bytes + cue_binding_infix.size() + 2 * digest256_width;
// Distinct cues one binding carries at most.
inline constexpr std::size_t max_bound_cues = 2048;

// The index views of experience (board §3B :122-123). Each record carries
// its entries; the journal's index tree answers a lookup by kind and value.
// - cue: every token of the source and its revision under the cue rule
//   (L3 hot cue index); a caller's authored cues are not the memory's and
//   are kept apart from its record (user 2026-09-23 18:0x), in cue
//   bindings that carry the same view;
// - source: looked up by the exact source text (the entry holds its
//   SHA-256); content: the SHA-256 of the raw bytes;
// - lineage: each address the experience was derived from (its derivations);
// - successor: the address it revises (validity and supersession: the
//   experience that supersedes an address is found here);
// - name_space, resource, transaction: the observation's namespace, each of
//   its resources, and the transaction it was appended in.
enum class ExperienceView : char {
    cue = 'c',
    source = 's',
    content = 'd',
    lineage = 'l',
    successor = 'v',
    name_space = 'n',
    resource = 'r',
    transaction = 't',
};

// Where in its source the raw bytes came from.
struct SourceSpan {
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
};

// Reads bytes an observation holds outside memory: `read(offset, out)`
// fills `out` exactly with the bytes at `offset`. Borrowed like a function
// reference (pass a callable object, never keep one); it must stay valid
// until the append it was given to is done.
class BlobReader final {
public:
    // Lineage: native mechanism — stores any reader of blob bytes by reference, without allocating; the author reads whole files.
    // SWEGCA: docs/SWEGCA_CPP_VRS_LAYER_PLAN.md@472d23225c973fa0a33581afd6bd9026df6fc98a:522-526
    // SWEGCA: user@2026-09-22:91-92
    template <class F>
        requires(!std::is_same_v<std::remove_cvref_t<F>, BlobReader> &&
                 std::is_object_v<std::remove_reference_t<F>> &&
                 std::is_invocable_v<std::remove_reference_t<F>&, std::uint64_t, std::span<std::byte>>)
    BlobReader(F&& read) noexcept  // NOLINT(google-explicit-constructor)
        : target_(static_cast<const void*>(std::addressof(read))),
          call_(&invoke<std::remove_reference_t<F>>) {}

    // Lineage: native mechanism — reads the bytes at an offset through the stored reader; the author reads whole files.
    // SWEGCA: docs/SWEGCA_CPP_VRS_LAYER_PLAN.md@472d23225c973fa0a33581afd6bd9026df6fc98a:522-526
    // SWEGCA: user@2026-09-22:91-92
    void operator()(std::uint64_t offset, std::span<std::byte> out) const { call_(target_, offset, out); }

private:
    using Call = void (*)(const void*, std::uint64_t, std::span<std::byte>);
    // Lineage: native mechanism — calls the stored reader through its erased type; the author reads whole files.
    // SWEGCA: docs/SWEGCA_CPP_VRS_LAYER_PLAN.md@472d23225c973fa0a33581afd6bd9026df6fc98a:522-526
    // SWEGCA: user@2026-09-22:91-92
    template <class T>
    static void invoke(const void* target, std::uint64_t offset, std::span<std::byte> out) {
        auto& read = *static_cast<T*>(const_cast<void*>(target));
        read(offset, out);
    }

    const void* target_;
    Call call_;
};

// Observed bytes of any size the storage holds: in memory, or `size` bytes
// read on demand through `reader` (bytes larger than memory). Read ones are
// read twice: in full to hash them when staged, and part by part when
// each part's generation is staged, checked against the first reading
// (`experience_source_changed`). Memory held is one part at a time, the
// part generation being staged, and the digest lists (1/262144 of the
// bytes).
struct BlobInput {
    BlobInput() = default;
    // Lineage: native mechanism — a blob given as bytes already in memory; the author reads whole files.
    // SWEGCA: docs/SWEGCA_CPP_VRS_LAYER_PLAN.md@472d23225c973fa0a33581afd6bd9026df6fc98a:522-526
    // SWEGCA: user@2026-09-22:91-92
    BlobInput(std::span<const std::byte> in_memory) noexcept  // NOLINT(google-explicit-constructor)
        : bytes(in_memory), size(in_memory.size()) {}
    // Lineage: native mechanism — a blob given as its size and a reader, so a blob larger than memory is read part by part; the author reads whole files.
    // SWEGCA: docs/SWEGCA_CPP_VRS_LAYER_PLAN.md@472d23225c973fa0a33581afd6bd9026df6fc98a:522-526
    // SWEGCA: user@2026-09-22:91-92
    BlobInput(std::uint64_t total, BlobReader read) noexcept : size(total), reader(read) {}

    std::span<const std::byte> bytes;
    std::uint64_t size = 0;
    std::optional<BlobReader> reader;
};

// A historical step's outcome: the user's six OUTCOMES.
enum class StepOutcome : std::uint8_t { success, failure, negative, uncertain, conflict, pending };

// What an observed resource is. Modality belongs to a resource, never to the
// whole memory.
enum class ResourceModality : std::uint8_t { text, image, audio, video };

// The pixel frame an image's native size is given in.
enum class CoordinateFrame : std::uint8_t { native_source_pixels, oriented_source_pixels };

// One historical step of a user episode (borrowed input). Texts are kept as
// given; `phase`, `judgment` and each evidence ref must hold something other
// than Python strip whitespace. `observation` is kept as given bytes.
struct MemoryStepInput {
    std::string_view phase;
    std::span<const std::byte> observation;
    std::span<const std::string_view> relations;  // kept in order, repeats kept, unchecked
    std::string_view judgment;
    StepOutcome outcome = StepOutcome::pending;
    std::span<const std::string_view> evidence_refs;  // at least one, in order, repeats kept
};

// A user episode carried by one memory (borrowed input). Its own revision is
// not the producer's `source_revision`. Its cues are not carried: cues are
// bound beside the memory (`CueBinding`).
struct MemoryEpisodeInput {
    std::string_view episode_id;  // the producer's id, kept as provenance
    std::string_view revision;
    std::string_view verification_state;
    std::span<const std::string_view> source_addresses;  // at least one, distinct, in order
    std::span<const MemoryStepInput> steps;              // at least one
};

// What an observed resource's native form is, by modality: a text's code
// point count; an image's native size and frame; audio's duration, alone or
// with the frames and sample rate that give it; a video's native size and
// duration.
struct ResourceDescriptor {
    ResourceModality modality = ResourceModality::text;
    std::uint64_t code_points = 0;
    std::uint64_t width = 0;
    std::uint64_t height = 0;
    CoordinateFrame frame = CoordinateFrame::native_source_pixels;
    std::uint64_t frames = 0;
    std::uint64_t sample_rate = 0;
    std::uint64_t duration_ns = 0;
};

// One resource a memory observed (borrowed input). Its bytes are kept only
// when they were received, as parts; a digest is kept only when computed from
// them or named by the source, and a locator only as provenance (never
// opened).
struct ObservedResourceInput {
    std::string_view resource_id;
    ResourceDescriptor descriptor;
    std::optional<DigestBytes> content_digest;
    std::optional<std::string_view> storage_locator;
    std::optional<std::uint64_t> item_index;  // the source's own value
    std::span<const std::byte> metadata;
    std::optional<BlobInput> bytes;
};

// One complete observation: the producer's borrowed input. Nothing is
// filtered (no success, verification or file-type test) and every field is
// kept. Before Main appends it, it has no address; appending gives it one
// and still grants nothing (board §4 :200-202).
struct Observation {
    std::string_view source;  // producer id
    std::string_view source_revision;
    std::uint64_t observed_at = 0;  // Main step it was observed at
    // The digest of the context it was observed in. Evidence must name the
    // same context (evidence_accumulator.hpp); an experience without one
    // cannot be evidence.
    std::optional<Digest256> context;
    std::optional<std::string_view> previous_revision_address;  // the experience this revises
    std::span<const std::string_view> derived_from;  // published experience addresses; empty for an original
    std::optional<std::string_view> outcome;
    double uncertainty = 0;    // [0, 1]
    double contradiction = 0;  // [0, 1]
    std::optional<SourceSpan> source_span;
    std::optional<std::string_view> name_space;   // the namespace it belongs to
    std::span<const std::string_view> resources;  // the resources it concerns, each once
    BlobInput raw;         // the exact bytes observed
    BlobInput structured;  // canonical structured form; empty when none
    // A user episode, when the observation is one; the resources it observed,
    // in the producer's order. Their ids join `resources` in the resource
    // view; each list is kept as given.
    std::optional<MemoryEpisodeInput> episode;
    std::span<const ObservedResourceInput> observed_resources;
    // No cue: an authored cue calls the memory and is not part of it (user
    // 2026-09-23 18:0x; the author's artifact has none, its semantic keys
    // come beside it, mosaic_unrestricted_experience.py@5901a5a:34-55,
    // :398-436). The tokens of the source and its revision are derived.
    // A caller's cues are bound beside it (`CueBinding`).
};

// A caller's cues for one memory (the caller's retrieval keys, beside the
// memory as the author's semantic postings are beside its artifact). Who
// authored them and at what revision is kept with them; equal bindings are
// one record. Binding grants nothing and changes no strength.
struct CueBinding {
    std::string_view target;  // a published memory's address, or one the same append gives
    std::string_view source;  // who authored the cues
    std::string_view source_revision;
    // Each kept whole, normalized as the user's `_cue` (strip, one space
    // per run, Unicode case folded); equal ones count once.
    std::span<const std::string_view> cues;
};

// The cue rule (author regex `n\d+|r\d+|[a-z]+|\d+|[^\W\d_]+` over lowered
// text), re-created natively as this module's own rule rather than
// emulating Python's `\w`: ASCII letters are lowered; a token is `n` or `r`
// followed by digits, a run of ASCII letters, a run of ASCII digits, or a run
// of letters starting with a non-ASCII letter (which continues through ASCII
// letters). A non-ASCII letter is any code point from U+00C0 outside the
// listed mark, punctuation, symbol, byte-order-mark and private blocks, so
// non-ASCII digits and marks count as letters or separators by that list,
// not by Unicode categories. Everything else separates tokens. Tokens view
// the lowered copy it keeps.
class CueTokens final {
public:
    // `source` must be strict UTF-8 (every identity text is).
    CueTokens(const AllocationContext& memory, std::string_view source);
    CueTokens(CueTokens&&) noexcept = default;
    CueTokens& operator=(CueTokens&&) = delete;
    CueTokens(const CueTokens&) = delete;
    CueTokens& operator=(const CueTokens&) = delete;
    ~CueTokens() = default;

    // In text order; a token may repeat.
    // SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:487-490
    [[nodiscard]] std::span<const std::string_view> tokens() const noexcept { return tokens_; }

private:
    journal::LedgerBytes text_;  // the lowered text; tokens view it
    journal::LedgerVector<std::string_view> tokens_;
};

// A visitor borrowed for one call, like a function reference: pass a
// callable object directly (a lambda or functor, not a plain function) and
// never keep one. It returns false to stop.
template <class Arg>
class ExperienceVisitor final {
public:
    // C++ infrastructure for reading the derived views (approved flow :62);
    // no direct Python counterpart.
    // SWEGCA: user@2026-09-22:62
    template <class F>
        requires(!std::is_same_v<std::remove_cvref_t<F>, ExperienceVisitor> &&
                 std::is_object_v<std::remove_reference_t<F>> &&
                 std::is_invocable_r_v<bool, std::remove_reference_t<F>&, Arg>)
    ExperienceVisitor(F&& visit) noexcept  // NOLINT(google-explicit-constructor)
        : target_(static_cast<const void*>(std::addressof(visit))),
          call_(&invoke<std::remove_reference_t<F>>) {}

    // SWEGCA: user@2026-09-22:62
    bool operator()(Arg value) const { return call_(target_, value); }

private:
    using Call = bool (*)(const void*, Arg);
    // SWEGCA: user@2026-09-22:62
    template <class T>
    static bool invoke(const void* target, Arg value) {
        auto& visit = *static_cast<T*>(const_cast<void*>(target));
        return static_cast<bool>(visit(value));
    }

    const void* target_;
    Call call_;
};

// Consecutive chunks of a blob's bytes, in order: an inline blob in one
// chunk, a parted one part by part.
using ChunkVisitor = ExperienceVisitor<std::span<const std::byte>>;
// Digests in increasing order: the root sources of an experience (SHA-256
// of each source text) or its root contexts.
using DigestVisitor = ExperienceVisitor<const DigestBytes&>;

// One blob of an experience record as the record holds it. Views the
// record's bytes. Inline (depth 0): `inline_bytes` holds all `size` bytes.
// Parted: the bytes are cut into parts of `experience_part_bytes` (the
// last may be shorter), each a record of its own (`experience_part_kind`)
// addressed by its SHA-256. The list of those digests is cut the same way
// when it is longer than an inline blob, level after level, until the top
// list fits: `depth` is the number of part levels (at most 3 for any u64
// size) and `top_digests` the top list, 32 bytes per part, in order. Every
// level's count follows from `size`, so a blob has one form.
struct ExperienceBlob {
    std::uint64_t size = 0;
    DigestBytes digest{};  // SHA-256 of all `size` bytes
    std::uint8_t depth = 0;
    std::span<const std::byte> inline_bytes;
    std::span<const std::byte> top_digests;

    // SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:23-60
    [[nodiscard]] bool parted() const noexcept { return depth != 0; }
};

// A decoded user episode, a step and an observed resource. Every text and
// byte span views the record's bytes; lists view the record's own lists.
struct EpisodeView {
    std::string_view episode_id;
    std::string_view revision;
    std::string_view verification_state;
    std::span<const std::string_view> source_addresses;
};

struct StepView {
    std::string_view phase;
    std::span<const std::byte> observation;
    std::span<const std::string_view> relations;
    std::string_view judgment;
    StepOutcome outcome = StepOutcome::pending;
    std::span<const std::string_view> evidence_refs;
};

struct ResourceView {
    std::string_view resource_id;
    ResourceDescriptor descriptor;
    std::optional<DigestBytes> content_digest;
    std::optional<std::string_view> storage_locator;
    std::optional<std::uint64_t> item_index;
    std::span<const std::byte> metadata;
    // The received bytes, inline or parted as every blob.
    std::optional<ExperienceBlob> bytes;
};

// Which field of the typed section an event carries.
enum class SectionPart : std::uint8_t {
    episode_id,
    revision,
    verification_state,
    source_address,
    phase,
    observation,
    relation,
    judgment,
    outcome,
    evidence_ref,
    resource,
    storage_locator,
    metadata,
    resource_bytes,
};

// One event of reading the typed section in order. A text or byte field
// comes in pieces (`piece`, the last with `last`) and is checked after its
// last piece; `outcome` carries a step's outcome; `resource` a resource's id
// and fixed fields, before its locator, metadata and bytes, and every later
// event of that resource carries them too. Received bytes kept inline come in
// pieces with their whole `size`; kept as parts, as one event with `parted`
// set and `piece` their top digest list; read those with
// `ExperienceRecord::for_each_resource_chunk`. Every span is valid only
// during the call.
struct SectionEvent {
    SectionPart part = SectionPart::episode_id;
    std::uint64_t step = 0;      // the step of a step field
    std::uint64_t resource = 0;  // the resource of a resource field
    std::uint64_t item = 0;      // the place in its list (source addresses, relations, evidence refs)
    std::span<const std::byte> piece;
    bool last = true;
    StepOutcome outcome = StepOutcome::pending;
    std::string_view resource_id;
    ResourceDescriptor descriptor;
    std::optional<DigestBytes> content_digest;
    std::optional<std::uint64_t> item_index;
    bool parted = false;
    std::uint64_t size = 0;  // resource_bytes: the whole size
};

// Visits section events in order; returning false stops the reading.
using SectionVisitor = ExperienceVisitor<const SectionEvent&>;

// One experience record read back: the published record Main replayed and
// the experience fields decoded from its payload. Every text and span it
// hands out views the record's bytes and is valid while this object lives.
// A parted blob is read part by part from the journal it was decoded with,
// which must outlive this object.
class ExperienceRecord final {
public:
    // Requires an experience kind without authority or claim, a well-formed
    // payload, an address that is the digest of the record's identity, and
    // index entries that are exactly the automatic ones.
    // Its lists are kept on `memory`; parts are replayed from `journal`.
    [[nodiscard]] static ExperienceRecord decode(journal::PublishedRecord record,
                                                 const AllocationContext& memory,
                                                 const journal::JournalStore& journal);

    // The source keeps nothing that views the bytes it gave away (like
    // PublishedRecord).
    ExperienceRecord(ExperienceRecord&& other) noexcept;
    ExperienceRecord& operator=(ExperienceRecord&&) = delete;
    ExperienceRecord(const ExperienceRecord&) = delete;
    ExperienceRecord& operator=(const ExperienceRecord&) = delete;
    ~ExperienceRecord() = default;

    // SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:23-60
    [[nodiscard]] const journal::RecordView& record() const noexcept { return record_.view(); }
    // SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:23-60
    [[nodiscard]] const journal::RecordPosition& position() const noexcept { return record_.position(); }
    // SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:23-60
    [[nodiscard]] bool derived() const noexcept {
        return record_.view().kind == derived_experience_kind;
    }
    // SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:23-60
    [[nodiscard]] std::uint64_t observed_at() const noexcept { return observed_at_; }
    // SWEGCA: paper/swegca/journal_submission_2026-08-25/COMPONENT_LEDGER.md@5901a5a:44-50
    [[nodiscard]] const std::optional<Digest256>& context() const noexcept { return context_; }
    // SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:179-205
    [[nodiscard]] double uncertainty() const noexcept { return uncertainty_; }
    // SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:179-205
    [[nodiscard]] double contradiction() const noexcept { return contradiction_; }
    // SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:23-60
    [[nodiscard]] const std::optional<SourceSpan>& source_span() const noexcept { return span_; }
    // SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:23-60
    [[nodiscard]] std::span<const std::string_view> derived_from() const noexcept {
        return derived_from_;
    }
    // SWEGCA: src/swegca/mosaic_external_memory.py@5901a5a:15-26
    [[nodiscard]] const std::optional<std::string_view>& name_space() const noexcept { return name_space_; }
    // Every resource the memory concerns, in increasing order: those the
    // producer listed and each observed resource's id.
    // SWEGCA: src/swegca/mosaic_external_memory.py@5901a5a:34-42
    [[nodiscard]] std::span<const std::string_view> resources() const noexcept { return resources_; }
    // Those the producer listed in `Observation::resources`, in increasing
    // order.
    // SWEGCA: src/swegca/mosaic_external_memory.py@5901a5a:34-42
    [[nodiscard]] std::span<const std::string_view> listed_resources() const noexcept { return listed_; }
    // Whether the memory has a typed section (a user episode or observed
    // resources), and whether that section is kept as parts. Only a section
    // kept in the record is decoded here, checked whole, and viewed by
    // `episode`, `steps` and `observed_resources`; a parted one is read with
    // `for_each_section` and checked by `verify_parts`, and those three are
    // then empty.
    // SWEGCA: src/tinylm_slicer/mosaic_memory_activation.py@3bddcb7:89-106
    [[nodiscard]] bool has_section() const noexcept { return has_section_; }
    // SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:137-155
    [[nodiscard]] bool section_parted() const noexcept { return has_section_ && section_.parted(); }
    // The user episode the memory carries, when it carries one, and its
    // steps in order.
    // SWEGCA: src/tinylm_slicer/mosaic_memory_activation.py@3bddcb7:89-106
    [[nodiscard]] const std::optional<EpisodeView>& episode() const noexcept { return episode_; }
    // SWEGCA: src/tinylm_slicer/mosaic_memory_activation.py@3bddcb7:63-87
    [[nodiscard]] std::span<const StepView> steps() const noexcept { return steps_; }
    // The resources it observed, in the producer's order.
    // SWEGCA: src/swegca/mosaic_external_memory.py@5901a5a:33-41
    [[nodiscard]] std::span<const ResourceView> observed_resources() const noexcept { return observed_; }
    // Reads the typed section in order, inline or parted, checking every
    // field as it goes (the checks `verify_parts` makes, received bytes kept
    // as parts apart: those are read by `for_each_resource_chunk` and
    // `verify_parts`); nothing when it has none. A resource's inline bytes
    // come in pieces and are checked against their digest after the last.
    // SWEGCA: src/tinylm_slicer/mosaic_memory_activation.py@3bddcb7:63-106
    void for_each_section(SectionVisitor visit) const;
    // SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:23-60
    [[nodiscard]] const ExperienceBlob& raw_blob() const noexcept { return raw_; }
    // The raw bytes of an inline blob; a parted one fails with
    // `experience_blob_parted` (read it with `for_each_raw_chunk`).
    // SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:23-60
    [[nodiscard]] std::span<const std::byte> raw() const;
    // SHA-256 of the raw bytes (the author's raw_sha256; the content view's key).
    // SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:520-529
    [[nodiscard]] const DigestBytes& raw_digest() const noexcept { return raw_.digest; }
    // SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:23-60
    [[nodiscard]] const ExperienceBlob& structured_blob() const noexcept { return structured_; }
    // Like `raw()`.
    // SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:23-60
    [[nodiscard]] std::span<const std::byte> structured() const;
    // The record's index entries (kind letter and value), increasing.
    // Weak: the user's postings per key, automatic and supplied; the kind
    // letters are C++'s.
    // SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:410-431
    [[nodiscard]] std::span<const std::string_view> index_entries() const noexcept { return index_; }

    // Streams the blob's bytes: each part is replayed, checked against its
    // digest and size and released after its bytes are visited, so at most
    // one part per level is held;
    // after the last, the whole blob is checked against its digest
    // (`experience_blob_digest_mismatch`). A part the journal does not
    // hold fails as Replay does; one that is not the part its parent names
    // fails with `experience_part_invalid`.
    // SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:137-155
    void for_each_raw_chunk(ChunkVisitor visit) const;
    // SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:137-155
    void for_each_structured_chunk(ChunkVisitor visit) const;
    // Streams the received bytes of observed resource `resource` (in the
    // producer's order) as `for_each_raw_chunk` does; one without bytes fails
    // with `experience_resource_bytes_absent`. In a parted section the
    // section is read up to it.
    // SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:137-155
    void for_each_resource_chunk(std::size_t resource, ChunkVisitor visit) const;
    // The sources every observation this experience rests on came from: its
    // own source for an original; for a derived one, the union of the root
    // sources of what it was derived from, fixed when it was appended.
    // SWEGCA: paper/swegca/journal_submission_2026-08-25/COMPONENT_LEDGER.md@5901a5a:44-50
    void for_each_root_source(DigestVisitor visit) const;
    // The contexts the observations it rests on were made in: its own for
    // an original (none when it has none); for a derived one, the union of
    // the root contexts of what it was derived from. A derived experience's
    // own context is what it was derived in, never evidence context.
    // SWEGCA: paper/swegca/journal_submission_2026-08-25/COMPONENT_LEDGER.md@5901a5a:44-50
    void for_each_root_context(DigestVisitor visit) const;
    // Reads every parted blob (raw, structured, root sources, root
    // contexts) and the typed section to its end, with each observed
    // resource's parted bytes (a text resource's checked as strict UTF-8 of
    // its code point count), checking each part, the order of every digest
    // set and each whole digest, as `for_each_raw_chunk` does; one part per
    // level is held. Evidence admission and Re-evidence run it before an
    // experience counts, so a missing or damaged part fails there.
    // SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:137-155
    void verify_parts() const;

private:
    ExperienceRecord(journal::PublishedRecord record, const AllocationContext& memory,
                     const journal::JournalStore& journal, journal::LedgerVector<std::string_view> derived,
                     journal::LedgerVector<std::string_view> resources,
                     journal::LedgerVector<std::string_view> index);
    void for_each_chunk(const ExperienceBlob& blob, ChunkVisitor visit) const;
    void for_each_digest(const ExperienceBlob& blob, DigestVisitor visit) const;

    journal::PublishedRecord record_;
    const journal::JournalStore* journal_;
    AllocationContext memory_;
    std::uint64_t observed_at_ = 0;
    std::optional<Digest256> context_;
    double uncertainty_ = 0;
    double contradiction_ = 0;
    std::optional<SourceSpan> span_;
    journal::LedgerVector<std::string_view> derived_from_;  // views the record's bytes
    std::optional<std::string_view> name_space_;             // views the record's bytes
    journal::LedgerVector<std::string_view> resources_;      // views the record's bytes
    journal::LedgerVector<std::string_view> index_;          // views the record's bytes
    ExperienceBlob raw_;
    ExperienceBlob structured_;
    ExperienceBlob roots_;     // derived only: the root-source digests, 32 bytes each
    ExperienceBlob contexts_;  // derived only: the root-context digests, 32 bytes each
    journal::LedgerVector<std::string_view> listed_;         // views the record's bytes
    bool has_section_ = false;
    ExperienceBlob section_;
    journal::LedgerVector<std::string_view> section_texts_;  // lists the views below view
    std::optional<EpisodeView> episode_;
    journal::LedgerVector<StepView> steps_;
    journal::LedgerVector<ResourceView> observed_;
};

// One cue binding read back. Its texts view the record's bytes and are
// valid while this object lives.
class CueBindingRecord final {
public:
    // Requires the cue-binding kind without authority, claim, revised
    // address or outcome, a well-formed payload, an address that is the
    // binding's digest under its target, index entries that are exactly its
    // cues' entries, and a target the journal holds as a memory (kind 1 or 2).
    [[nodiscard]] static CueBindingRecord decode(journal::PublishedRecord record, const AllocationContext& memory,
                                                 const journal::JournalStore& journal);
    // The same checks for a binding a view rebuild meets before it is
    // published, its target read through the rebuilt view (`reader`).
    // SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:398-436
    static void validate_rebuilt(const journal::RecordView& record, const journal::RecordPosition& position,
                                 const journal::RebuildReader& reader, const AllocationContext& memory);

    CueBindingRecord(CueBindingRecord&& other) noexcept = default;
    CueBindingRecord& operator=(CueBindingRecord&&) = delete;
    CueBindingRecord(const CueBindingRecord&) = delete;
    CueBindingRecord& operator=(const CueBindingRecord&) = delete;
    ~CueBindingRecord() = default;

    // SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:398-436
    [[nodiscard]] const journal::RecordView& record() const noexcept { return record_.view(); }
    // SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:398-436
    [[nodiscard]] const journal::RecordPosition& position() const noexcept { return record_.position(); }
    // The memory's address.
    // SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:398-436
    [[nodiscard]] std::string_view target() const noexcept { return target_; }
    // Normalized phrases, increasing, distinct.
    // SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:425-427
    [[nodiscard]] std::span<const std::string_view> cues() const noexcept { return cues_; }

private:
    // SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:398-436
    CueBindingRecord(journal::PublishedRecord record, std::string_view target,
                     journal::LedgerVector<std::string_view> cues) noexcept
        : record_(std::move(record)), target_(target), cues_(std::move(cues)) {}

    journal::PublishedRecord record_;
    std::string_view target_;                     // views the record's bytes
    journal::LedgerVector<std::string_view> cues_;  // views the record's bytes
};

class ExperienceJournal;

namespace detail {

// A typed section being appended, read as one blob without copying the
// caller's bytes: its encoded fields and length prefixes (`encoded`) and the
// caller's bytes between them, in order. (One small enough to be inline, up
// to `experience_inline_blob_bytes`, is then read once into the record.) It reads itself through `input`, so
// it must stay where it was built (its owner reserves room for every one
// before building any, and moving the owner's list keeps its buffer).
struct SectionSource {
    struct Piece {
        std::uint64_t at = 0;            // where it starts in the section
        std::uint64_t encoded_from = 0;  // where it is in `encoded`, when `caller` is empty
        std::uint64_t length = 0;
        std::span<const std::byte> caller;
    };

    // C++ infrastructure for streaming a section larger than memory holds
    // (approved flow :91-92); no direct Python counterpart.
    // SWEGCA: user@2026-09-22:91-92
    explicit SectionSource(const AllocationContext& memory)
        : encoded(memory.allocator<std::byte>()), pieces(memory.allocator<Piece>()) {}

    // Fills `out` with the section's bytes at `offset`.
    // SWEGCA: user@2026-09-22:91-92
    void operator()(std::uint64_t offset, std::span<std::byte> out) const;

    journal::LedgerBytes encoded;
    journal::LedgerVector<Piece> pieces;
    std::uint64_t size = 0;
    std::optional<BlobInput> input;
};

}  // namespace detail

// What appending staged, handed out one generation at a time. The parts of
// every new experience come first, then the experience records, then the
// cue bindings, so an experience becomes visible only with its record,
// after all its parts, and a binding only once its memory is:
// a crash in between leaves only unreferenced parts, which an append-only
// journal keeps harmlessly and a retry reuses (equal parts are one record).
// `next` stages the next generation on the state and views Main passes;
// Main publishes it before calling `next` again. A generation that was not
// published (its publication failed) is staged again by the next call, so
// `done` means every record is in the journal.
// Every experience is appended whole in one generation; experiences are
// spread over as many generations as the journal's generation limit needs.
// Borrows every input `ExperienceJournal::stage` was given until `done`.
class ExperienceAppend final {
public:
    ExperienceAppend(ExperienceAppend&&) noexcept = default;
    ExperienceAppend& operator=(ExperienceAppend&&) = delete;
    ExperienceAppend(const ExperienceAppend&) = delete;
    ExperienceAppend& operator=(const ExperienceAppend&) = delete;
    ~ExperienceAppend() = default;

    // One address per observation, in input order.
    // SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:31-35
    [[nodiscard]] std::span<const ExperienceAddress> addresses() const noexcept { return addresses_; }
    // True once nothing is left to stage.
    // C++ infrastructure for admitting observations (approved flow :61);
    // no direct Python counterpart.
    // SWEGCA: user@2026-09-22:61
    [[nodiscard]] bool done() const noexcept { return done_; }
    // The next generation, or none once everything is staged (an
    // observation already in the journal is not appended again; one found
    // there with other index entries, which its fields all derive, fails
    // `experience_index_conflict`).
    [[nodiscard]] std::optional<journal::StagedGeneration> next(const StateGeneration& state,
                                                                std::span<const journal::ViewGeneration> views);

private:
    friend class ExperienceJournal;
    struct Head {
        std::uint16_t kind = 0;
        std::size_t observation = 0;  // index into `observations_`
        std::array<char, experience_address_bytes> address{};
        journal::LedgerBytes payload;
        journal::LedgerVector<std::string_view> index;  // views `index_bytes`
        journal::LedgerBytes index_bytes;
        bool skip = false;  // equal to an earlier head of this append
        // The record digest this append last staged for it, until its
        // generation is confirmed (as for parts).
        std::optional<DigestBytes> staged;
    };
    struct Part {
        DigestBytes digest{};
        std::array<char, experience_part_address_bytes> address{};
        std::span<const std::byte> bytes;  // the caller's bytes or `owned_`, or
        const BlobReader* reader = nullptr;  // read at `offset` when staged
        std::uint64_t offset = 0;
        std::uint64_t length = 0;
        // Its address resolves to this very part: the record this append
        // staged is the one published (same record digest), or the one
        // published was checked record for record.
        bool known = false;
        // The record digest this append last staged for it, until confirmed.
        std::optional<DigestBytes> staged;

        // SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:31-35
        auto operator<=>(const Part& other) const noexcept { return digest <=> other.digest; }
        // SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:31-35
        bool operator==(const Part& other) const noexcept { return digest == other.digest; }
    };
    struct Binding {
        std::size_t input = 0;  // index into `binding_inputs_`
        std::array<char, cue_binding_address_bytes> address{};
        journal::LedgerVector<std::string_view> cues;  // distinct, increasing; view the caller's
        // Encoded when staged: it names where the target was published,
        // which a target this append gives is only then.
        journal::LedgerBytes payload;
        journal::LedgerVector<std::string_view> index;  // views `index_bytes`
        journal::LedgerBytes index_bytes;
        bool skip = false;  // equal to an earlier binding of this append, or already bound
        std::optional<DigestBytes> staged;  // as for heads
    };
    // C++ infrastructure for admitting observations (approved flow :61);
    // no direct Python counterpart.
    // SWEGCA: user@2026-09-22:61
    ExperienceAppend(const ExperienceJournal& journal, const AllocationContext& memory,
                     std::span<const Observation> observations, std::span<const CueBinding> bindings,
                     std::string_view operation_id, std::optional<std::string_view> transaction_id);
    [[nodiscard]] journal::RecordDraft head_draft(const Head& head) const;
    [[nodiscard]] journal::RecordDraft binding_draft(const Binding& binding) const;

    const ExperienceJournal* journal_;
    AllocationContext memory_;
    std::span<const Observation> observations_;
    std::string_view operation_id_;
    std::optional<std::string_view> transaction_id_;
    journal::LedgerVector<Head> heads_;
    journal::LedgerVector<Part> parts_;  // sorted by digest, unique
    journal::LedgerVector<journal::LedgerBytes> owned_;  // bytes parts view that the caller did not give
    // Typed sections read in place; room for one per observation is
    // reserved first, so none ever moves (parts keep their readers).
    journal::LedgerVector<detail::SectionSource> sections_;
    journal::LedgerVector<ExperienceAddress> addresses_;
    std::span<const CueBinding> binding_inputs_;
    journal::LedgerVector<Binding> bindings_;
    std::size_t next_part_ = 0;
    // The generation `next` last returned: 1 parts, 2 records, 3 bindings,
    // 0 none, and where it began. The next call rewinds to it unless all of
    // it is published.
    std::uint8_t pending_ = 0;
    std::size_t pending_from_ = 0;
    std::size_t next_head_ = 0;
    std::size_t next_binding_ = 0;
    bool parts_checked_ = false;
    bool done_ = false;
};

// Main's original-experience journal (board §5 :282-283): stages complete
// observations as records under digest-bound addresses, replays them, and
// answers the index views. The address is the digest of the record's
// identity (kind, source, revision, revised address, outcome and payload
// digest; the payload holds every other observed field and the digest of
// every part), so the same observation appended twice has one address and
// one record. Index entries are not identity but are derived from it, so an
// existing observation found with other entries fails with
// `experience_index_conflict`, and one appended again in another
// transaction keeps the transaction it was first appended in.
class ExperienceJournal final {
public:
    ExperienceJournal(const ExperienceJournal&) = delete;
    ExperienceJournal& operator=(const ExperienceJournal&) = delete;
    ExperienceJournal(ExperienceJournal&&) = delete;
    ExperienceJournal& operator=(ExperienceJournal&&) = delete;
    ~ExperienceJournal() = default;

    // Validates every observation (texts by the identity rule, uncertainty
    // and contradiction finite in [0, 1], resources unique, every lineage
    // address a published experience), computes
    // each derived one's root sources from its lineage, encodes each with
    // its index entries and splits every blob larger than
    // `experience_inline_blob_bytes` into parts, all before anything is
    // staged, under Main's `operation_id` and, when given, `transaction_id`.
    // The returned append stages the generations (`ExperienceAppend::next`).
    [[nodiscard]] ExperienceAppend stage(std::span<const Observation> observations,
                                         std::string_view operation_id,
                                         std::optional<std::string_view> transaction_id) const;
    // As above, and binds each caller's cues beside its memory once every
    // memory is published. A target is a published memory (kind 1 or 2) or
    // an address this append gives (`experience_cue_binding_target_unknown`);
    // each cue is kept whole in its normalized form (empty or invalid:
    // `experience_cue_invalid`),
    // at least one and at most `max_bound_cues` distinct per binding.
    [[nodiscard]] ExperienceAppend stage(std::span<const Observation> observations,
                                         std::span<const CueBinding> bindings, std::string_view operation_id,
                                         std::optional<std::string_view> transaction_id) const;

    // Replays one exact experience (`journal_address_unknown` when absent).
    [[nodiscard]] ExperienceRecord replay(const ExperienceAddress& address) const;

    // The state generation named by the current published journal HEAD.
    [[nodiscard]] StateGeneration state_generation() const;

    // Visits, in address order over one published snapshot, every experience
    // `view` names for `key` until `visit` returns false: for `cue` a single
    // cue in its kept form: a token of the memory cue rule or a whole bound
    // phrase (it visits memories and the cue bindings beside them, whose
    // address begins with their memory's); for `source`, `name_space`, `resource` and `transaction` the
    // exact text; for `content` the 64 lowercase hex digits of the raw
    // bytes' SHA-256; for `lineage` and `successor` an experience address.
    // Another key fails with `experience_view_key_invalid`.
    void for_each_in_view(ExperienceView view, std::string_view key, journal::IndexVisitor visit) const;

private:
    friend class MainOwner;
    friend class ExperienceSelector;
    friend class ExperienceAppend;
    // C++ infrastructure for admitting observations (approved flow :61);
    // no direct Python counterpart.
    // SWEGCA: user@2026-09-22:61
    ExperienceJournal(const journal::JournalStore& journal, const AllocationContext& memory) noexcept
        : journal_(journal), memory_(memory) {}

    const journal::JournalStore& journal_;
    AllocationContext memory_;
};

// q of Select(q, U): the query text, tokenized by the cue rule, and the
// digest of the context the query was asked in.
struct SelectionQuery {
    std::string_view text;
    Digest256 context;
};

// What the judge sees of one candidate: its address, exact position and how
// many distinct query cues retrieved it. The address lives for the call only.
struct SelectionCandidate {
    std::string_view address;
    journal::RecordPosition position;
    std::uint32_t matched_cues = 0;
};

// One judgment of a retrieved candidate (author ExperienceCandidateJudgment
// :179-205), as the judge hands it to the sink: borrowed for that call.
struct CandidateVerdict {
    std::string_view address;
    bool selected = false;
    double relevance = 0;      // [0, 1]
    double contradiction = 0;  // [0, 1]
    std::string_view verification_state;
    std::string_view revision;
    std::string_view rationale;
    std::span<const std::string_view> rejection_evidence;  // each nonempty
};

struct VerificationStateTag { static constexpr std::string_view name = "verification_state"; };
struct RevisionTextTag { static constexpr std::string_view name = "revision"; };
struct RationaleTag { static constexpr std::string_view name = "rationale"; };
struct EvidenceTextTag { static constexpr std::string_view name = "rejection_evidence"; };
struct QueryTextTag { static constexpr std::string_view name = "query"; };
using VerificationState = TextIdentity<VerificationStateTag>;
using RevisionText = TextIdentity<RevisionTextTag>;
using Rationale = TextIdentity<RationaleTag>;
using EvidenceText = TextIdentity<EvidenceTextTag>;
using QueryText = TextIdentity<QueryTextTag>;

// J: one kept judgment, in Main's allocation context.
struct CandidateJudgment {
    ExperienceAddress address;
    journal::RecordPosition position;
    std::uint32_t matched_cues = 0;
    bool selected = false;
    double relevance = 0;
    double contradiction = 0;
    VerificationState verification_state;
    RevisionText revision;
    Rationale rationale;
    journal::LedgerVector<EvidenceText> rejection_evidence;
};

// Where the judge puts its verdict on one candidate. `record` copies every
// text into Main's allocation context before it returns, so nothing the judge hands over
// has to outlive that call. A judge records exactly one verdict per
// candidate: none fails with `experience_judgment_missing`, a second with
// `experience_judgment_repeated`, another address with
// `experience_judgment_address_changed`.
class VerdictSink final {
public:
    VerdictSink(const VerdictSink&) = delete;
    VerdictSink& operator=(const VerdictSink&) = delete;
    VerdictSink(VerdictSink&&) = delete;
    VerdictSink& operator=(VerdictSink&&) = delete;
    ~VerdictSink() = default;

    void record(const CandidateVerdict& verdict);

private:
    friend class ExperienceSelector;
    // SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:179-205
    VerdictSink(const AllocationContext& memory, const SelectionCandidate& candidate) noexcept
        : memory_(memory), candidate_(candidate) {}

    const AllocationContext& memory_;
    const SelectionCandidate& candidate_;
    std::optional<CandidateJudgment> judgment_;
};

// The runtime cognition judgment over one candidate, borrowed for one
// selection like a function reference: pass a callable object directly (a
// lambda or functor, not a plain function) and never keep one. It is called
// as judge(candidate, query, sink) and records its verdict in `sink`.
class SelectionJudge final {
public:
    // SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:475-512
    template <class F>
        requires(!std::is_same_v<std::remove_cvref_t<F>, SelectionJudge> &&
                 std::is_object_v<std::remove_reference_t<F>> &&
                 std::is_invocable_v<std::remove_reference_t<F>&, const SelectionCandidate&,
                                     const SelectionQuery&, VerdictSink&>)
    SelectionJudge(F&& judge) noexcept  // NOLINT(google-explicit-constructor)
        : target_(static_cast<const void*>(std::addressof(judge))),
          call_(&invoke<std::remove_reference_t<F>>) {}

    // SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:508-512
    void operator()(const SelectionCandidate& candidate, const SelectionQuery& query,
                    VerdictSink& sink) const {
        call_(target_, candidate, query, sink);
    }

private:
    using Call = void (*)(const void*, const SelectionCandidate&, const SelectionQuery&, VerdictSink&);
    // SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:508-512
    template <class T>
    static void invoke(const void* target, const SelectionCandidate& candidate,
                       const SelectionQuery& query, VerdictSink& sink) {
        auto& judge = *static_cast<T*>(const_cast<void*>(target));
        judge(candidate, query, sink);
    }

    const void* target_;
    Call call_;
};

// C: one selected experience with its replay handle (exact position) and
// what Replay verified of it (author selected artifact :520-529).
struct SelectedExperience {
    ExperienceAddress address;
    journal::RecordPosition position;
    std::uint64_t byte_count = 0;  // raw bytes of the experience
    DigestBytes raw_digest{};      // SHA-256 of those bytes (author raw_sha256)
    DigestBytes record_digest{};
    // Parted raw bytes: `byte_count` and `raw_digest` are the record's, bound
    // to its address; the parts are checked when the bytes are replayed
    // (`for_each_raw_chunk`), not by the selection, which stays within the
    // Recall time bound.
    bool raw_parted = false;
    VerificationState verification_state;
    RevisionText revision;
};

// U: the published journal generation a selection ran over.
using SelectionUniverse = journal::PublishedUniverse;

template <class Authority>
class SelectionReceipt;  // defined for NoAuthority only (board §4 :207-210)

class ExperienceSelector;

// rho: the replayable receipt of Rozephine judging every retrieved candidate
// (author RuntimeExperienceSelectionReceipt :208-290). Its static authority
// is NoAuthority (strong_types.hpp), the only one a receipt is defined for,
// and nothing converts it to another: it audits, it authorizes nothing, and
// its digest covers every field and the flags below.
template <>
class SelectionReceipt<NoAuthority> final {
public:
    using Authority = NoAuthority;
    // The author fixes both receipt flags and validates them on construction.
    // SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:218-225
    // SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:245-247
    static constexpr bool rozephine_selected = true;
    static constexpr bool codex_per_item_approval_used = false;
    static constexpr bool external_action_authorized = false;
    static constexpr bool memory_write_authorized = false;
    static constexpr bool world_write_authorized = false;
    static constexpr bool training_write_authorized = false;
    static constexpr bool p3_promotion_authorized = false;

    SelectionReceipt(SelectionReceipt&&) noexcept = default;
    SelectionReceipt& operator=(SelectionReceipt&&) = delete;
    SelectionReceipt(const SelectionReceipt&) = delete;
    SelectionReceipt& operator=(const SelectionReceipt&) = delete;
    ~SelectionReceipt() = default;

    // SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:208-243
    [[nodiscard]] std::string_view query() const noexcept { return query_.value(); }
    // SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:208-243
    [[nodiscard]] const Digest256& context() const noexcept { return context_; }
    // SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:208-243
    [[nodiscard]] const SelectionUniverse& universe() const noexcept { return universe_; }
    // SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:208-243
    [[nodiscard]] std::span<const CandidateJudgment> judgments() const noexcept { return judgments_; }
    // SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:208-243
    [[nodiscard]] std::span<const SelectedExperience> selected() const noexcept { return selected_; }
    // SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:266-272
    [[nodiscard]] static constexpr std::string_view method() noexcept {
        return "runtime_cognition_relevance_and_contradiction_judgment";
    }
    // SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:530-532
    [[nodiscard]] static constexpr std::string_view rationale() noexcept {
        return "Rozephine selected relevant experience after explicit contradiction review";
    }
    // SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:274-290
    [[nodiscard]] const Digest256& digest() const noexcept { return digest_; }

private:
    friend class ExperienceSelector;
    SelectionReceipt(QueryText query, const Digest256& context, const SelectionUniverse& universe,
                     journal::LedgerVector<CandidateJudgment> judgments,
                     journal::LedgerVector<SelectedExperience> selected);
    [[nodiscard]] Digest256 compute_digest() const;

    QueryText query_;
    Digest256 context_;
    SelectionUniverse universe_;
    journal::LedgerVector<CandidateJudgment> judgments_;  // in address order
    journal::LedgerVector<SelectedExperience> selected_;  // the selected judgments, in order
    Digest256 digest_;
};

// Main's limits on one selection. More retrieved entries than
// `max_retrieved` fail closed (`experience_select_over_policy`) rather than
// being cut: no retrieved candidate is ever dropped unjudged. Every index
// entry a lookup returns counts, a memory's own and each cue binding's
// beside it alike: the bound is on the work of one selection.
struct SelectionPolicy {
    std::uint32_t max_retrieved = 1u << 16;
};

// Select(q, U) -> (C, J, rho) (board §3B :124-126). Retrieval derives
// candidates from the query's cue tokens through the cue view of the
// published journal U (never by scanning every experience, board §9
// :592-595); the judge, Main's runtime cognition, judges every candidate;
// each selected one is replayed exactly and verified. There is no fallback
// to the whole universe: no candidate fails with
// `experience_select_no_candidates`, and no selection with
// `experience_select_nothing_selected`, as the author's.
class ExperienceSelector final {
public:
    ExperienceSelector(const ExperienceSelector&) = delete;
    ExperienceSelector& operator=(const ExperienceSelector&) = delete;
    ExperienceSelector(ExperienceSelector&&) = delete;
    ExperienceSelector& operator=(ExperienceSelector&&) = delete;
    ~ExperienceSelector() = default;

    [[nodiscard]] SelectionReceipt<NoAuthority> select(const SelectionQuery& query,
                                                       SelectionJudge judge) const;

private:
    friend class MainOwner;
    // C++ infrastructure for the read route (approved flow :20-24);
    // no direct Python counterpart.
    // SWEGCA: user@2026-09-22:20-24
    ExperienceSelector(const ExperienceJournal& experience, SelectionPolicy policy) noexcept
        : experience_(experience), policy_(policy) {}

    const ExperienceJournal& experience_;
    SelectionPolicy policy_;
};

}  // namespace swegca::vrs
