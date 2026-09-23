#include "swegca_vrs/state_recovery.hpp"

#include "swegca_vrs/blob_field.hpp"
#include "swegca_vrs/core_sha256.hpp"
#include "swegca_vrs/native_tensor.hpp"
#include "swegca_vrs/part_tree.hpp"
#include "swegca_vrs/state_publication_codec.hpp"
#include "swegca_vrs/state_record_codec.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace swegca::vrs {

// Lineage: native mechanism — keep every state record read on one journal snapshot.
// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3fce8b5c605166d668924baa5d4a6c49dc0:587-590
PinnedJournalStateSource::PinnedJournalStateSource(journal::JournalReadSnapshot pinned) noexcept
    : pinned_(std::move(pinned)) {}

// Lineage: native mechanism — address resolution uses the pinned view.
// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3fce8b5c605166d668924baa5d4a6c49dc0:569-570
std::optional<journal::RecordPosition> PinnedJournalStateSource::resolve(
    std::string_view address) const {
    return pinned_.resolve(address);
}

// Lineage: native mechanism — exact position reading uses that same view.
// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3fce8b5c605166d668924baa5d4a6c49dc0:569-570
journal::PublishedRecord PinnedJournalStateSource::read_at(
    const journal::RecordPosition& position) const {
    return pinned_.read_at(position);
}

namespace {

using Bytes = part_tree::Bytes;

// The canonical stream's domain, the first bytes of its prefix section.
constexpr std::string_view content_domain = "swegca.cognitive_state.content.v4";
constexpr std::string_view transition_prefix = "state-transition:";
constexpr std::size_t prefix_section = 0;
constexpr std::size_t entities_section = 4;
constexpr std::size_t relations_section = 5;
constexpr std::size_t evidence_section = 6;
constexpr std::size_t final_fields_section = 7;

// SWEGCA: paper/swegca/ARCHITECTURE_SPEC.md@5901a5a:196-205
[[noreturn]] void fail(const char* step) { throw std::invalid_argument(step); }

// Lowercase hex of a digest.
// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3fce8b5c605166d668924baa5d4a6c49dc0:567-572
std::array<char, 2 * digest256_width> hex_of(const DigestBytes& digest) noexcept {
    constexpr std::string_view hex = "0123456789abcdef";
    std::array<char, 2 * digest256_width> out{};
    std::size_t at = 0;
    for (const auto byte : digest) {
        const auto value = std::to_integer<unsigned>(byte);
        out[at++] = hex[value >> 4];
        out[at++] = hex[value & 0x0f];
    }
    return out;
}

// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3fce8b5c605166d668924baa5d4a6c49dc0:567-572
template <std::size_t N>
std::string_view view_of(const std::array<char, N>& text) noexcept {
    return std::string_view(text.data(), text.size());
}

// A state record carries Main's source and nothing that would make it an
// experience, a claim or an authority: no authority, claim, outcome,
// transaction, previous revision or index entry.
// Lineage: native mechanism — Main-only state records (kinds 5-7) are fixed-form storage.
// SWEGCA: paper/swegca/ARCHITECTURE_SPEC.md@5901a5a:17
void check_state_record(const journal::RecordView& view, std::uint16_t kind,
                        std::string_view owner, const char* step) {
    if (view.kind != kind || view.authority || !view.claim.empty() || !view.outcome.empty() ||
        !view.transaction_id.empty() || !view.previous_revision_address.empty() ||
        view.index_count != 0 || view.source != owner)
        fail(step);
}

// The record the pinned address view holds for `address`, read at exactly
// that position and naming exactly that address.
// Lineage: native mechanism — an exact address resolves to one immutable record position.
// SWEGCA: docs/SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md@cefdc3fce8b5c605166d668924baa5d4a6c49dc0:569-570
journal::PublishedRecord read_exact(const StateRecordSource& source, std::string_view address,
                                    const char* step) {
    const auto position = source.resolve(address);
    if (!position) fail(step);
    auto record = source.read_at(*position);
    if (record.position() != *position || record.view().address != address) fail(step);
    return record;
}

// One kind-5 part, checked against the digest and length its parent names.
// Lineage: weak analogy — the author re-stats an artifact and refuses a changed size; here a part must carry exactly the digest and length its parent names.
// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:103-104
// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:109-110
journal::PublishedRecord read_part(const StateRecordSource& source, std::string_view owner,
                                   const DigestBytes& digest, std::uint64_t expected) {
    const auto address = state_part_address(digest);
    auto part = read_exact(source, view_of(address), "state_recovery_invalid:part");
    const auto& view = part.view();
    check_state_record(view, journal::state_part_record_kind, owner, "state_recovery_invalid:part");
    if (view.payload_digest != digest || view.payload.size() != expected)
        fail("state_recovery_invalid:part");
    return part;
}

// SWEGCA: docs/SWEGCA_CPP_VRS_LAYER_PLAN.md@472d23225c973fa0a33581afd6bd9026df6fc98a:522-523
DigestBytes digest_at(std::span<const std::byte> list, std::uint64_t at) noexcept {
    DigestBytes out{};
    std::memcpy(out.data(), list.data() + at * digest256_width, digest256_width);
    return out;
}

// Reads parted data forward, holding one part per part-tree level, each read
// and checked as it is reached; `finish` requires every byte read and, when
// a whole digest is named, that digest.
// Lineage: native mechanism — the experience blob cursor's walk over the shared part tree, with Main's exact-record reads.
// SWEGCA: docs/SWEGCA_CPP_VRS_LAYER_PLAN.md@472d23225c973fa0a33581afd6bd9026df6fc98a:522-526
// SWEGCA: user@2026-09-22:91-92
class PartCursor final {
public:
    // SWEGCA: user@2026-09-22:91-92
    PartCursor(const StateRecordSource& source, std::string_view owner, std::uint64_t size,
               std::uint8_t depth, std::span<const std::byte> top,
               std::optional<DigestBytes> whole)
        : source_(&source), owner_(owner), size_(size), levels_(part_tree::part_levels(size)),
          whole_digest_(whole) {
        if (depth != levels_.depth) fail("state_recovery_invalid:descriptor");
        lists_[depth - 1].list = top;
        if (whole_digest_) whole_.emplace();
    }

    // SWEGCA: user@2026-09-22:91-92
    [[nodiscard]] std::uint64_t consumed() const noexcept { return read_; }
    // SWEGCA: user@2026-09-22:91-92
    [[nodiscard]] std::uint64_t remaining() const noexcept { return size_ - read_; }

    // Up to `limit` of the bytes that follow, at least one: the rest of the
    // current part at most.
    // SWEGCA: user@2026-09-22:91-92
    std::span<const std::byte> next(std::uint64_t limit) {
        if (limit == 0 || remaining() == 0) fail("state_recovery_invalid:section");
        if (chunk_.empty()) load();
        const auto take = static_cast<std::size_t>(std::min<std::uint64_t>(limit, chunk_.size()));
        const auto out = chunk_.first(take);
        chunk_ = chunk_.subspan(take);
        read_ += take;
        if (whole_) whole_->update(out);
        return out;
    }

    // Every byte read and, when named, the whole digest; then the held parts
    // are released.
    // SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:109-110
    void finish() {
        if (remaining() != 0) fail("state_recovery_invalid:section");
        if (whole_ && whole_->finish() != *whole_digest_) fail("state_recovery_invalid:section");
        whole_.reset();
        leaf_.reset();
        for (auto& level : lists_) level.part.reset();
        chunk_ = {};
    }

private:
    struct Level {
        std::optional<journal::PublishedRecord> part;
        std::span<const std::byte> list;
        std::uint64_t at = 0;
    };

    // The next level-0 part: up from the lowest level with a digest left,
    // then down, each list part replacing the one before it at its level.
    // SWEGCA: docs/SWEGCA_CPP_VRS_LAYER_PLAN.md@472d23225c973fa0a33581afd6bd9026df6fc98a:522-523
    void load() {
        std::uint8_t level = 0;
        while (level < levels_.depth && lists_[level].at * digest256_width == lists_[level].list.size())
            ++level;
        if (level == levels_.depth) fail("state_recovery_invalid:part");
        while (true) {
            auto& list = lists_[level];
            const auto digest = digest_at(list.list, list.at++);
            auto part = read_part(*source_, owner_, digest,
                                  state_part_length(size_, level, placed_[level]++));
            if (level == 0) {
                leaf_.reset();
                leaf_.emplace(std::move(part));
                chunk_ = leaf_->view().payload;
                return;
            }
            --level;
            auto& below = lists_[level];
            below.part.reset();
            below.part.emplace(std::move(part));
            below.list = below.part->view().payload;
            below.at = 0;
        }
    }

    const StateRecordSource* source_;
    std::string_view owner_;
    std::uint64_t size_;
    part_tree::PartLevels levels_;
    std::optional<DigestBytes> whole_digest_;
    std::optional<Sha256> whole_;
    std::array<Level, 4> lists_{};
    std::array<std::uint64_t, 4> placed_{};
    std::optional<journal::PublishedRecord> leaf_;
    std::span<const std::byte> chunk_;
    std::uint64_t read_ = 0;
};

// A tensor's canonical bytes, streamed from its checked parts. The native
// tensor reads them in order; any other offset reads nothing, which the
// tensor rejects as a short read. At the end it reads nothing (EOF) and
// releases the held parts.
// Lineage: native mechanism — the bounded startup reader MainInitialState already routes to CognitiveTensor.
// SWEGCA: user@2026-09-22:91-92
class TensorPartReader final : public TensorByteReader {
public:
    // SWEGCA: user@2026-09-22:91-92
    TensorPartReader(const StateRecordSource& source, std::string_view owner,
                     const StateSectionDescriptor& descriptor)
        : cursor_(source, owner, descriptor.data.size, descriptor.data.depth,
                  descriptor.data.top_digests, std::nullopt) {}

    // SWEGCA: user@2026-09-22:91-92
    [[nodiscard]] std::size_t read(std::uint64_t offset,
                                   std::span<std::byte> destination) const override {
        if (offset != cursor_.consumed()) return 0;
        if (cursor_.remaining() == 0) {
            cursor_.finish();
            return 0;
        }
        std::size_t filled = 0;
        while (filled < destination.size() && cursor_.remaining() != 0) {
            const auto piece = cursor_.next(destination.size() - filled);
            std::memcpy(destination.data() + filled, piece.data(), piece.size());
            filled += piece.size();
        }
        return filled;
    }

private:
    mutable PartCursor cursor_;
};

// A bounds-checked reader of one plain section's canonical bytes: the
// emitter's little-endian u8/u64 and u64-length-prefixed texts and bytes.
// Lineage: direct — the inverse of the emitter's fields (cognitive_state.cpp emit_u8/emit_u64/emit_bytes/emit_text).
// SWEGCA: src/swegca/mosaic_bounded_world_write.py@5901a5a:262-283
class SectionReader final {
public:
    // SWEGCA: src/swegca/mosaic_bounded_world_write.py@5901a5a:262-283
    explicit SectionReader(std::span<const std::byte> bytes) noexcept : bytes_(bytes) {}

    // SWEGCA: user@2026-09-22:60-61
    std::uint8_t u8() { return std::to_integer<std::uint8_t>(take(1)[0]); }
    // SWEGCA: user@2026-09-22:60-61
    std::uint64_t u64() {
        const auto bytes = take(8);
        std::uint64_t value = 0;
        for (std::size_t at = 8; at-- > 0;)
            value = (value << 8) | std::to_integer<std::uint64_t>(bytes[at]);
        return value;
    }
    // SWEGCA: src/swegca/mosaic_bounded_world_write.py@5901a5a:262-283
    std::span<const std::byte> bytes() {
        const auto length = u64();
        if (length > remaining()) fail("state_recovery_invalid:section");
        return take(static_cast<std::size_t>(length));
    }
    // SWEGCA: src/swegca/mosaic_bounded_world_write.py@5901a5a:262-283
    std::string_view text() {
        const auto value = bytes();
        return std::string_view(reinterpret_cast<const char*>(value.data()), value.size());
    }
    // SWEGCA: src/swegca/mosaic_bounded_world_write.py@5901a5a:262-283
    std::span<const std::byte> take(std::size_t count) {
        if (count > remaining()) fail("state_recovery_invalid:section");
        const auto out = bytes_.subspan(at_, count);
        at_ += count;
        return out;
    }
    // A count of items each at least `minimum` bytes long, bounded by what
    // is left, so no count reserves more than the section could hold.
    // SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:109-110
    std::size_t count(std::size_t minimum) {
        const auto value = u64();
        if (value > remaining() / minimum) fail("state_recovery_invalid:section");
        return static_cast<std::size_t>(value);
    }
    // SWEGCA: src/swegca/mosaic_bounded_world_write.py@5901a5a:262-283
    [[nodiscard]] std::size_t remaining() const noexcept { return bytes_.size() - at_; }
    // Every byte of the section consumed.
    // SWEGCA: src/swegca/mosaic_bounded_world_write.py@5901a5a:262-283
    void finish() const {
        if (remaining() != 0) fail("state_recovery_invalid:section");
    }

private:
    std::span<const std::byte> bytes_;
    std::size_t at_ = 0;
};

}  // namespace

// Everything the recovered input views, in one host-charged allocation that
// does not move, so the MainInitialState spans and reader pointers stay valid
// when the handle moves.
struct RecoveredStateInput::Storage final {
    // SWEGCA: user@2026-09-22:91-92
    explicit Storage(const AllocationContext& memory)
        : roles(memory.allocator<RoleDefinitionInput>()),
          entities(memory.allocator<WorldEntityInput>()),
          entity_evidence(memory.allocator<std::string_view>()),
          relations(memory.allocator<WorldRelationInput>()),
          evidence(memory.allocator<std::string_view>()) {}

    std::array<std::optional<journal::PublishedRecord>, state_section_count> descriptor_records;
    std::array<StateSectionDescriptor, state_section_count> descriptors{};
    std::array<std::optional<Bytes>, state_section_count> plain_copies;
    std::array<std::span<const std::byte>, state_section_count> plain{};
    std::vector<RoleDefinitionInput, AllocationAdapter<RoleDefinitionInput>> roles;
    std::vector<WorldEntityInput, AllocationAdapter<WorldEntityInput>> entities;
    std::vector<std::string_view, AllocationAdapter<std::string_view>> entity_evidence;
    std::vector<WorldRelationInput, AllocationAdapter<WorldRelationInput>> relations;
    std::vector<std::string_view, AllocationAdapter<std::string_view>> evidence;
    std::array<std::optional<TensorPartReader>, 3> tensors;
    MainInitialState input{};
    DigestBytes content{};
    journal::RecordPosition publication{};
};

// SWEGCA: user@2026-09-22:91-92
RecoveredStateInput::RecoveredStateInput(std::shared_ptr<Storage> storage) noexcept
    : storage_(std::move(storage)) {}

// SWEGCA: user@2026-09-22:91-92
const MainInitialState& RecoveredStateInput::input() const noexcept { return storage_->input; }

// SWEGCA: paper/swegca/ARCHITECTURE_SPEC.md@5901a5a:196-205
const DigestBytes& RecoveredStateInput::content_digest() const noexcept {
    return storage_->content;
}

// SWEGCA: paper/swegca/ARCHITECTURE_SPEC.md@5901a5a:196-205
const journal::RecordPosition& RecoveredStateInput::publication() const noexcept {
    return storage_->publication;
}

namespace {

// R1: the kind-7 record at the manifest's position is a genesis selection of
// the manifest's content, at the exact address its payload gives, with the
// metadata of that one transition.
// Lineage: native mechanism — Main's selected publication record names the state it committed.
// SWEGCA: src/tinylm_slicer/mosaic_paper_resident_assimilation.py@3bddcb7:491-535
DigestBytes check_publication(const StateRecordSource& source, std::string_view owner,
                              const journal::StateHeadReference& head) {
    constexpr const char* step = "state_recovery_invalid:publication";
    if (!head.publication || head.content_digest == DigestBytes{}) fail(step);
    const auto& position = *head.publication;
    const auto record = source.read_at(position);
    if (record.position() != position) fail(step);
    const auto& view = record.view();
    check_state_record(view, journal::state_publication_record_kind, owner, step);
    const auto content = decode_genesis_state_publication(view.payload);
    if (content != head.content_digest) fail(step);
    const auto address = genesis_state_publication_address(content);
    if (view.address != view_of(address)) fail(step);
    const auto resolved = source.resolve(view_of(address));
    if (!resolved || *resolved != position) fail(step);
    if (view.source_revision != view_of(hex_of(content))) fail(step);
    const auto transition = hex_of(Sha256::of(view.payload));
    if (view.operation_id.size() != transition_prefix.size() + transition.size() ||
        view.operation_id.substr(0, transition_prefix.size()) != transition_prefix ||
        view.operation_id.substr(transition_prefix.size()) != view_of(transition))
        fail(step);
    return content;
}

// The bytes of plain section `section`: an inline one in its descriptor
// record, a parted one copied from its checked parts into one host-charged
// buffer and checked against its whole digest.
// SWEGCA: user@2026-09-22:91-92
std::span<const std::byte> plain_bytes(RecoveredStateInput::Storage& storage,
                                       const StateRecordSource& source, std::string_view owner,
                                       std::size_t section, const AllocationContext& memory) {
    const auto& data = storage.descriptors[section].data;
    if (!data.parted()) return data.inline_bytes;
    PartCursor cursor(source, owner, data.size, data.depth, data.top_digests, data.digest);
    auto& copy = storage.plain_copies[section].emplace(memory.allocator<std::byte>());
    copy.reserve(static_cast<std::size_t>(data.size));
    while (cursor.remaining() != 0) {
        const auto piece = cursor.next(cursor.remaining());
        copy.insert(copy.end(), piece.begin(), piece.end());
    }
    cursor.finish();
    return copy;
}

// R5, prefix: the domain, then the owner, which must be Main's, then roles.
// SWEGCA: src/swegca/mosaic_bounded_world_write.py@5901a5a:262-283
void parse_prefix(RecoveredStateInput::Storage& storage, std::string_view owner) {
    SectionReader reader(storage.plain[prefix_section]);
    const auto domain = reader.take(content_domain.size());
    if (!std::equal(domain.begin(), domain.end(),
                    reinterpret_cast<const std::byte*>(content_domain.data())))
        fail("state_recovery_invalid:domain");
    storage.input.owner = reader.text();
    if (storage.input.owner != owner) fail("state_recovery_invalid:owner");
    const auto roles = reader.count(8 + 1 + 8);
    storage.roles.reserve(roles);
    for (std::size_t at = 0; at < roles; ++at) {
        const auto id = reader.text();
        const auto partition = reader.u8();
        if (partition < static_cast<std::uint8_t>(TensorPartition::semantic) ||
            partition > static_cast<std::uint8_t>(TensorPartition::scratch))
            fail("state_recovery_invalid:section");
        storage.roles.push_back({id, static_cast<TensorPartition>(partition), reader.u64()});
    }
    reader.finish();
    storage.input.roles = storage.roles;
}

// R5, entities, relations and evidence, in the emitter's order.
// SWEGCA: src/swegca/mosaic_bounded_world_write.py@5901a5a:262-283
void parse_graph(RecoveredStateInput::Storage& storage, const AllocationContext& memory) {
    SectionReader entities(storage.plain[entities_section]);
    const auto entity_count = entities.count(8 + 8 + 8 + 8 + 8);
    storage.entities.reserve(entity_count);
    std::vector<std::pair<std::size_t, std::size_t>,
                AllocationAdapter<std::pair<std::size_t, std::size_t>>>
        spans(memory.allocator<std::pair<std::size_t, std::size_t>>());
    for (std::size_t at = 0; at < entity_count; ++at) {
        WorldEntityInput entity{};
        entity.id = entities.text();
        entity.kind = entities.text();
        entity.properties = entities.bytes();
        entity.spatial = entities.bytes();
        const auto references = entities.count(8);
        const auto first = storage.entity_evidence.size();
        for (std::size_t item = 0; item < references; ++item)
            storage.entity_evidence.push_back(entities.text());
        storage.entities.push_back(entity);
        spans.emplace_back(first, references);
    }
    entities.finish();
    // Views into the evidence list only once it no longer grows.
    const std::span<const std::string_view> all(storage.entity_evidence);
    for (std::size_t at = 0; at < entity_count; ++at)
        storage.entities[at].evidence_references = all.subspan(spans[at].first, spans[at].second);
    storage.input.entities = storage.entities;

    SectionReader relations(storage.plain[relations_section]);
    const auto relation_count = relations.count(8 + 8 + 8 + 8);
    storage.relations.reserve(relation_count);
    for (std::size_t at = 0; at < relation_count; ++at) {
        WorldRelationInput relation{};
        relation.subject = relations.text();
        relation.predicate = relations.text();
        relation.object = relations.text();
        relation.properties = relations.bytes();
        storage.relations.push_back(relation);
    }
    relations.finish();
    storage.input.relations = storage.relations;

    SectionReader evidence(storage.plain[evidence_section]);
    const auto evidence_count = evidence.count(8);
    storage.evidence.reserve(evidence_count);
    for (std::size_t at = 0; at < evidence_count; ++at) storage.evidence.push_back(evidence.text());
    evidence.finish();
    storage.input.evidence_references = storage.evidence;
}

// R5, final fields: goals, values and self payloads; a genesis state has no
// bounded-write head, so its flag must be zero.
// SWEGCA: src/swegca/mosaic_bounded_world_write.py@5901a5a:262-283
void parse_final_fields(RecoveredStateInput::Storage& storage) {
    SectionReader reader(storage.plain[final_fields_section]);
    storage.input.goals = reader.bytes();
    storage.input.values = reader.bytes();
    storage.input.self = reader.bytes();
    if (reader.u8() != 0) fail("state_recovery_invalid:write_head");
    reader.finish();
}

// R5, tensors: the header's type and shape, and a reader over the parts.
// SWEGCA: src/swegca/mosaic_cognitive_kernel.py@5901a5a:175-254
MainInitialState::TensorInput tensor_input(RecoveredStateInput::Storage& storage,
                                           const StateRecordSource& source,
                                           std::string_view owner, std::size_t section) {
    const auto& descriptor = storage.descriptors[section];
    const auto header = descriptor.tensor_header;
    const auto u64_at = [header](std::size_t offset) {
        std::uint64_t value = 0;
        for (std::size_t at = 8; at-- > 0;)
            value = (value << 8) | std::to_integer<std::uint64_t>(header[offset + at]);
        return value;
    };
    const auto& reader = storage.tensors[section - 1].emplace(source, owner, descriptor);
    MainInitialState::TensorInput input{};
    input.scalar_type = static_cast<ScalarType>(std::to_integer<std::uint8_t>(header[1]));
    input.shape = TensorShape3{u64_at(3), u64_at(11), u64_at(19)};
    input.reader = &reader;
    return input;
}

}  // namespace

// R1-R5 in order: the publication, the root, all eight descriptors, then the
// plain sections and the tensor readers. Nothing below a record is read
// before that record is checked.
// SWEGCA: src/tinylm_slicer/mosaic_paper_resident_assimilation.py@3bddcb7:491-535
RecoveredStateInput recover_genesis_state(const StateRecordSource& source,
                                          std::string_view expected_owner,
                                          const journal::StateHeadReference& manifest_head,
                                          const AllocationContext& memory) {
    if (expected_owner.empty()) fail("state_recovery_invalid:owner");
    const auto content = check_publication(source, expected_owner, manifest_head);

    const auto root_address = state_root_address(content);
    const auto root_record =
        read_exact(source, view_of(root_address), "state_recovery_invalid:root");
    check_state_record(root_record.view(), journal::state_root_record_kind, expected_owner,
                       "state_recovery_invalid:root");
    // A root is keyed by its content and every writer that stages it names
    // that content as its revision. Its operation id is not checked: the
    // root is reused by any later transition to the same content.
    if (root_record.view().source_revision != view_of(hex_of(content)))
        fail("state_recovery_invalid:root");
    const auto root = decode_state_root(root_record.view().payload);
    if (root.content_digest != content) fail("state_recovery_invalid:root");

    auto storage = std::allocate_shared<RecoveredStateInput::Storage>(
        memory.allocator<RecoveredStateInput::Storage>(), memory);
    storage->content = content;
    storage->publication = *manifest_head.publication;
    for (std::size_t section = 0; section < state_section_count; ++section) {
        const auto& entry = root.sections[section];
        const auto address = state_part_address(entry.descriptor);
        auto& record = storage->descriptor_records[section].emplace(
            read_exact(source, view_of(address), "state_recovery_invalid:descriptor"));
        const auto& view = record.view();
        check_state_record(view, journal::state_part_record_kind, expected_owner,
                           "state_recovery_invalid:descriptor");
        if (view.payload_digest != entry.descriptor) fail("state_recovery_invalid:descriptor");
        storage->descriptors[section] =
            decode_state_descriptor(view.payload, section, entry.section_bytes);
    }

    for (const auto section : {prefix_section, entities_section, relations_section,
                               evidence_section, final_fields_section})
        storage->plain[section] = plain_bytes(*storage, source, expected_owner, section, memory);
    parse_prefix(*storage, expected_owner);
    parse_graph(*storage, memory);
    parse_final_fields(*storage);
    // The owner view the part checks keep is the parsed one, which lives in
    // this storage as long as the tensor readers do.
    storage->input.semantic = tensor_input(*storage, source, storage->input.owner, 1);
    storage->input.executive = tensor_input(*storage, source, storage->input.owner, 2);
    storage->input.scratch = tensor_input(*storage, source, storage->input.owner, 3);
    return RecoveredStateInput(std::move(storage));
}

}  // namespace swegca::vrs
