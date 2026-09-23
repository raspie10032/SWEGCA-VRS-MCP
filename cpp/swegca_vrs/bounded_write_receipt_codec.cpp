#include "swegca_vrs/bounded_write_receipt_codec.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace swegca::vrs {
namespace {

// SWEGCA: user@2026-09-22:60-61
[[noreturn]] void invalid(std::string_view rule) {
    throw std::invalid_argument("bounded_write_receipt_invalid:" + std::string(rule));
}

// The slot's byte count for its type and width, or a refusal when the type
// is not one of the four or the count overflows.
// Lineage: direct — the author's receipt dtypes and its [1, width] slot.
// SWEGCA: src/tinylm_slicer/mosaic_bounded_world_write.py@3bddcb7:246-248
std::uint64_t slot_byte_count(ScalarType type, std::uint64_t width) {
    const auto value = static_cast<std::uint8_t>(type);
    if (value < static_cast<std::uint8_t>(ScalarType::bfloat16) ||
        value > static_cast<std::uint8_t>(ScalarType::float64))
        invalid("slot_type");
    const auto element = static_cast<std::uint64_t>(scalar_width(type));
    if (width > std::numeric_limits<std::uint64_t>::max() / element) invalid("slot_width");
    return width * element;
}

// Every recorded field of the publication is nonzero, as the fields of a
// Main-published identity are; this is data, not that identity.
// Lineage: native mechanism — the same structural refusal PublishedStateId's constructor makes.
// SWEGCA: user@2026-09-22:60-61
bool publication_present(const DigestBytes& content, const journal::RecordPosition& at) noexcept {
    return content != DigestBytes{} && at.segment_ordinal != 0 && at.byte_offset != 0 &&
           at.sequence != 0 && at.record_digest != DigestBytes{};
}

// SWEGCA: user@2026-09-22:60-61
void put_u8(StateContentSink sink, std::uint8_t value) {
    const std::array<std::byte, 1> bytes{static_cast<std::byte>(value)};
    sink(bytes);
}

// SWEGCA: user@2026-09-22:60-61
void put_u64(StateContentSink sink, std::uint64_t value) {
    std::array<std::byte, 8> bytes{};
    for (std::size_t at = 0; at < bytes.size(); ++at)
        bytes[at] = static_cast<std::byte>((value >> (8 * at)) & 0xff);
    sink(bytes);
}

// SWEGCA: user@2026-09-22:60-61
void put_digest(StateContentSink sink, const DigestBytes& digest) { sink(digest); }

// SWEGCA: user@2026-09-22:60-61
void put_text(StateContentSink sink, std::string_view text) {
    put_u64(sink, text.size());
    if (!text.empty()) sink(std::as_bytes(std::span<const char>(text.data(), text.size())));
}

// SWEGCA: user@2026-09-22:60-61
void put_evidence(StateContentSink sink, const EvidenceReferences& evidence) {
    put_u64(sink, evidence.size());
    for (const auto& address : evidence) put_text(sink, address.value());
}

}  // namespace

// Lineage: direct — every field bounded_world_write_receipt_to_dict writes,
// the before-slot as its dtype and exact element bytes, plus the C++ audit
// fields; the byte form is native.
// SWEGCA: src/tinylm_slicer/mosaic_bounded_world_write.py@3bddcb7:243-270
void encode_bounded_write_receipt(const BoundedWriteReceipt& receipt, StateContentSink sink) {
    // Checked before any byte is written, so a refusal writes nothing.
    // PublishedStateId already refuses a zero field at construction.
    const auto slot_bytes = slot_byte_count(receipt.before_slot_type, receipt.slot_width);
    if (receipt.before_slot.size() != slot_bytes) invalid("slot_length");
    if (detail::slot_digest(receipt.before_slot_type, receipt.slot_width, receipt.before_slot) !=
        receipt.before_slot_hash)
        invalid("before_slot_hash");
    if (detail::bounded_write_receipt_id(receipt.before_state_hash, receipt.applied_delta_hash,
                                         receipt.revision, receipt.evidence_references, receipt.claim,
                                         receipt.proposal_digest) != receipt.receipt_id)
        invalid("receipt_id");
    if (receipt.authority_domain != AuthorityDomain::cognitive_state_commit) invalid("authority_domain");

    sink(std::as_bytes(std::span<const char>(bounded_write_receipt_magic.data(),
                                             bounded_write_receipt_magic.size())));
    const std::array<std::byte, 2> version{
        static_cast<std::byte>(bounded_write_receipt_version & 0xff),
        static_cast<std::byte>(bounded_write_receipt_version >> 8)};
    sink(version);
    put_digest(sink, receipt.receipt_id.bytes());
    put_u64(sink, receipt.revision);
    put_text(sink, receipt.target_role.value());
    put_digest(sink, receipt.before_state_hash.bytes());
    put_digest(sink, receipt.after_state_hash.bytes());
    put_u8(sink, static_cast<std::uint8_t>(receipt.before_slot_type));
    put_u64(sink, receipt.slot_width);
    put_digest(sink, receipt.before_slot_hash.bytes());
    put_digest(sink, receipt.after_slot_hash.bytes());
    put_digest(sink, receipt.stored_slot_hash.bytes());
    put_digest(sink, receipt.applied_delta_hash.bytes());
    put_evidence(sink, receipt.evidence_references);
    put_u8(sink, receipt.prior_write_head ? 1 : 0);
    if (receipt.prior_write_head) {
        const auto& head = *receipt.prior_write_head;
        put_text(sink, head.policy_version.value());
        put_digest(sink, head.receipt_id.bytes());
        put_u64(sink, head.revision);
        put_text(sink, head.target_role.value());
        put_evidence(sink, head.evidence_references);
        put_text(sink, head.claim.claim().value());
        put_u64(sink, head.claim.revision());
        put_digest(sink, head.proposal_digest.bytes());
    }
    put_text(sink, receipt.claim.claim().value());
    put_u64(sink, receipt.claim.revision());
    put_digest(sink, receipt.proposal_digest.bytes());
    const auto& publication = receipt.before_publication.publication();
    put_digest(sink, receipt.before_publication.content_digest().bytes());
    put_u64(sink, publication.segment_ordinal);
    put_u64(sink, publication.byte_offset);
    put_u64(sink, publication.sequence);
    put_digest(sink, publication.record_digest);
    put_digest(sink, receipt.decision_digest.bytes());
    put_digest(sink, receipt.binding_digest.bytes());
    put_digest(sink, receipt.binding_receipt.bytes());
    put_digest(sink, receipt.preview_receipt.bytes());
    put_u8(sink, static_cast<std::uint8_t>(receipt.authority_domain));
    put_u64(sink, slot_bytes);
    if (slot_bytes != 0) sink(receipt.before_slot);
}

// Lineage: direct — bounded_world_write_receipt_from_dict reads back every
// field and its dtype allowlist; the expected shape, the slot and id checks
// and the stream form are native.
// SWEGCA: src/tinylm_slicer/mosaic_bounded_world_write.py@3bddcb7:273-306
BoundedWriteReceiptParser::BoundedWriteReceiptParser(const AllocationContext& memory,
                                                     ReceiptSlotShape expected)
    : memory_(memory), expected_(expected),
      expected_slot_bytes_(slot_byte_count(expected.type, expected.width)),
      text_(memory.allocator<char>()), evidence_(memory.allocator<ExperienceAddress>()),
      prior_evidence_(memory.allocator<ExperienceAddress>()), slot_(memory.allocator<std::byte>()) {}

// SWEGCA: src/tinylm_slicer/mosaic_bounded_world_write.py@3bddcb7:273-306
void BoundedWriteReceiptParser::feed(std::span<const std::byte> bytes) {
    if (field_ == Field::spent) invalid("spent");
    if (broken_) invalid("failed");
    broken_ = true;
    stream_hash_.update(bytes);
    while (!bytes.empty()) {
        if (field_ == Field::done) invalid("trailing_bytes");
        consume(bytes);
    }
    broken_ = false;
}

// Takes what the current field still needs from the front of `bytes`.
// SWEGCA: user@2026-09-22:60-61
void BoundedWriteReceiptParser::consume(std::span<const std::byte>& bytes) {
    if (field_ == Field::slot_bytes) {
        const auto take = static_cast<std::size_t>(
            std::min<std::uint64_t>(slot_left_, static_cast<std::uint64_t>(bytes.size())));
        const auto piece = bytes.first(take);
        slot_.insert(slot_.end(), piece.begin(), piece.end());
        slot_hash_->update(piece);
        slot_left_ -= take;
        bytes = bytes.subspan(take);
        if (slot_left_ == 0) begin_field(Field::done);
        return;
    }
    if (is_text(field_) && !text_length_) {
        const auto take = static_cast<std::size_t>(
            std::min<std::uint64_t>(text_left_, static_cast<std::uint64_t>(bytes.size())));
        const auto piece = bytes.first(take);
        text_.append(reinterpret_cast<const char*>(piece.data()), piece.size());
        text_left_ -= take;
        bytes = bytes.subspan(take);
        if (text_left_ == 0) on_text();
        return;
    }
    const auto width = is_text(field_) ? std::size_t{8} : fixed_width(field_);
    const auto take = std::min(width - fixed_have_, bytes.size());
    std::copy_n(bytes.begin(), take, fixed_.begin() + static_cast<std::ptrdiff_t>(fixed_have_));
    fixed_have_ += take;
    bytes = bytes.subspan(take);
    if (fixed_have_ != width) return;
    if (!is_text(field_)) {
        on_fixed();
        return;
    }
    // A text's length: bounded by the identity rule before any byte is kept.
    const auto length = fixed_u64();
    if (length > detail::identity_text_max_bytes) invalid("text_length");
    text_.clear();
    text_left_ = length;
    text_length_ = false;
    if (length == 0) on_text();
}

// SWEGCA: user@2026-09-22:60-61
void BoundedWriteReceiptParser::begin_field(Field field) {
    field_ = field;
    fixed_have_ = 0;
    text_length_ = true;
}

// A list of evidence references, kept in order; nothing is reserved from the
// count, so memory grows only with the references actually received.
// SWEGCA: src/tinylm_slicer/mosaic_bounded_world_write.py@3bddcb7:262
void BoundedWriteReceiptParser::after_evidence_count(std::uint64_t count, bool prior) {
    evidence_left_ = count;
    if (count != 0)
        begin_field(prior ? Field::prior_evidence : Field::evidence);
    else
        begin_field(prior ? Field::prior_claim : Field::prior_flag);
}

// A completed text: an identity-typed value, checked by its constructor.
// SWEGCA: src/tinylm_slicer/mosaic_bounded_world_write.py@3bddcb7:292-306
void BoundedWriteReceiptParser::on_text() {
    const std::string_view text(text_.data(), text_.size());
    switch (field_) {
        case Field::target_role:
            target_role_.emplace(memory_, text);
            begin_field(Field::before_state);
            return;
        case Field::evidence:
            evidence_.emplace_back(memory_, text);
            begin_field(--evidence_left_ == 0 ? Field::prior_flag : Field::evidence);
            return;
        case Field::prior_policy:
            prior_policy_.emplace(memory_, text);
            begin_field(Field::prior_receipt_id);
            return;
        case Field::prior_role:
            prior_role_.emplace(memory_, text);
            begin_field(Field::prior_evidence_count);
            return;
        case Field::prior_evidence:
            prior_evidence_.emplace_back(memory_, text);
            begin_field(--evidence_left_ == 0 ? Field::prior_claim : Field::prior_evidence);
            return;
        case Field::prior_claim:
            prior_claim_.emplace(memory_, text);
            begin_field(Field::prior_claim_revision);
            return;
        case Field::claim:
            claim_.emplace(memory_, text);
            begin_field(Field::claim_revision);
            return;
        default:
            invalid("state");
    }
}

// A completed fixed-width field.
// SWEGCA: src/tinylm_slicer/mosaic_bounded_world_write.py@3bddcb7:273-306
void BoundedWriteReceiptParser::on_fixed() {
    switch (field_) {
        case Field::magic:
            if (!std::equal(fixed_.begin(), fixed_.begin() + 8,
                            reinterpret_cast<const std::byte*>(bounded_write_receipt_magic.data())))
                invalid("magic");
            begin_field(Field::version);
            return;
        case Field::version:
            if (std::to_integer<std::uint16_t>(fixed_[0]) +
                    (std::to_integer<std::uint16_t>(fixed_[1]) << 8) !=
                bounded_write_receipt_version)
                invalid("version");
            begin_field(Field::receipt_id);
            return;
        case Field::receipt_id: receipt_id_.emplace(fixed_digest()); begin_field(Field::revision); return;
        case Field::revision: revision_ = fixed_u64(); begin_field(Field::target_role); return;
        case Field::before_state: before_state_.emplace(fixed_digest()); begin_field(Field::after_state); return;
        case Field::after_state: after_state_.emplace(fixed_digest()); begin_field(Field::slot_type); return;
        case Field::slot_type:
            if (std::to_integer<std::uint8_t>(fixed_[0]) != static_cast<std::uint8_t>(expected_.type))
                invalid("slot_type");
            begin_field(Field::slot_width);
            return;
        case Field::slot_width:
            if (fixed_u64() != expected_.width) invalid("slot_width");
            slot_width_ = expected_.width;
            begin_field(Field::before_slot_hash);
            return;
        case Field::before_slot_hash:
            before_slot_hash_.emplace(fixed_digest());
            begin_field(Field::after_slot_hash);
            return;
        case Field::after_slot_hash:
            after_slot_hash_.emplace(fixed_digest());
            begin_field(Field::stored_slot_hash);
            return;
        case Field::stored_slot_hash:
            stored_slot_hash_.emplace(fixed_digest());
            begin_field(Field::applied_delta_hash);
            return;
        case Field::applied_delta_hash:
            applied_delta_hash_.emplace(fixed_digest());
            begin_field(Field::evidence_count);
            return;
        case Field::evidence_count: after_evidence_count(fixed_u64(), false); return;
        case Field::prior_flag: {
            const auto flag = std::to_integer<std::uint8_t>(fixed_[0]);
            if (flag > 1) invalid("prior_flag");
            has_prior_ = flag == 1;
            begin_field(has_prior_ ? Field::prior_policy : Field::claim);
            return;
        }
        case Field::prior_receipt_id:
            prior_receipt_id_.emplace(fixed_digest());
            begin_field(Field::prior_revision);
            return;
        case Field::prior_revision: prior_revision_ = fixed_u64(); begin_field(Field::prior_role); return;
        case Field::prior_evidence_count: after_evidence_count(fixed_u64(), true); return;
        case Field::prior_claim_revision:
            prior_claim_revision_ = fixed_u64();
            begin_field(Field::prior_proposal);
            return;
        case Field::prior_proposal: prior_proposal_.emplace(fixed_digest()); begin_field(Field::claim); return;
        case Field::claim_revision: claim_revision_ = fixed_u64(); begin_field(Field::proposal_digest); return;
        case Field::proposal_digest:
            proposal_digest_.emplace(fixed_digest());
            begin_field(Field::publication_content);
            return;
        case Field::publication_content:
            std::copy_n(fixed_.begin(), fixed_.size(), publication_.content_digest.begin());
            begin_field(Field::publication_segment);
            return;
        case Field::publication_segment:
            publication_.publication.segment_ordinal = fixed_u64();
            begin_field(Field::publication_offset);
            return;
        case Field::publication_offset:
            publication_.publication.byte_offset = fixed_u64();
            begin_field(Field::publication_sequence);
            return;
        case Field::publication_sequence:
            publication_.publication.sequence = fixed_u64();
            begin_field(Field::publication_record);
            return;
        case Field::publication_record:
            std::copy_n(fixed_.begin(), fixed_.size(), publication_.publication.record_digest.begin());
            if (!publication_present(publication_.content_digest, publication_.publication))
                invalid("before_publication");
            begin_field(Field::decision);
            return;
        case Field::decision: decision_.emplace(fixed_digest()); begin_field(Field::binding); return;
        case Field::binding: binding_.emplace(fixed_digest()); begin_field(Field::binding_receipt); return;
        case Field::binding_receipt:
            binding_receipt_.emplace(fixed_digest());
            begin_field(Field::preview);
            return;
        case Field::preview: preview_.emplace(fixed_digest()); begin_field(Field::authority); return;
        case Field::authority:
            if (std::to_integer<std::uint8_t>(fixed_[0]) !=
                static_cast<std::uint8_t>(AuthorityDomain::cognitive_state_commit))
                invalid("authority_domain");
            begin_field(Field::slot_length);
            return;
        case Field::slot_length:
            // The shape was checked above; the length must be exactly its bytes.
            if (fixed_u64() != expected_slot_bytes_) invalid("slot_length");
            slot_.reserve(static_cast<std::size_t>(expected_slot_bytes_));
            slot_hash_.emplace(expected_.type, expected_.width);
            slot_left_ = expected_slot_bytes_;
            begin_field(slot_left_ == 0 ? Field::done : Field::slot_bytes);
            return;
        default:
            invalid("state");
    }
}

// SWEGCA: user@2026-09-22:60-61
bool BoundedWriteReceiptParser::is_text(Field field) noexcept {
    switch (field) {
        case Field::target_role:
        case Field::evidence:
        case Field::prior_policy:
        case Field::prior_role:
        case Field::prior_evidence:
        case Field::prior_claim:
        case Field::claim:
            return true;
        default:
            return false;
    }
}

// SWEGCA: user@2026-09-22:60-61
std::size_t BoundedWriteReceiptParser::fixed_width(Field field) noexcept {
    switch (field) {
        case Field::magic: return 8;
        case Field::version: return 2;
        case Field::slot_type:
        case Field::prior_flag:
        case Field::authority: return 1;
        case Field::receipt_id:
        case Field::before_state:
        case Field::after_state:
        case Field::before_slot_hash:
        case Field::after_slot_hash:
        case Field::stored_slot_hash:
        case Field::applied_delta_hash:
        case Field::prior_receipt_id:
        case Field::prior_proposal:
        case Field::proposal_digest:
        case Field::publication_content:
        case Field::publication_record:
        case Field::decision:
        case Field::binding:
        case Field::binding_receipt:
        case Field::preview: return 32;
        default: return 8;
    }
}

// SWEGCA: user@2026-09-22:60-61
std::uint64_t BoundedWriteReceiptParser::fixed_u64() const noexcept {
    std::uint64_t value = 0;
    for (std::size_t at = 0; at < 8; ++at)
        value |= std::to_integer<std::uint64_t>(fixed_[at]) << (8 * at);
    return value;
}

// SWEGCA: user@2026-09-22:60-61
Digest256 BoundedWriteReceiptParser::fixed_digest() const {
    DigestBytes bytes{};
    std::copy_n(fixed_.begin(), bytes.size(), bytes.begin());
    return Digest256(bytes);
}

// Lineage: direct — from_dict's result, every field; the slot digest and the
// receipt id are rechecked here with the writer's own preimages.
// SWEGCA: src/tinylm_slicer/mosaic_bounded_world_write.py@3bddcb7:273-306
StoredBoundedWriteReceipt BoundedWriteReceiptParser::finish() {
    const auto reached = field_;
    field_ = Field::spent;  // spent now, and failed for good if a check throws
    if (reached == Field::spent) invalid("spent");
    if (broken_) invalid("failed");
    if (reached != Field::done) invalid("truncated");
    if (slot_hash_->finish() != *before_slot_hash_) invalid("before_slot_hash");
    ClaimRevision claim(std::move(*claim_), claim_revision_);
    if (detail::bounded_write_receipt_id(*before_state_, *applied_delta_hash_, revision_, evidence_, claim,
                                         *proposal_digest_) != *receipt_id_)
        invalid("receipt_id");
    std::optional<BoundedWriteHead> prior;
    if (has_prior_)
        prior.emplace(BoundedWriteHead{std::move(*prior_policy_), *prior_receipt_id_, prior_revision_,
                                       std::move(*prior_role_), std::move(prior_evidence_),
                                       ClaimRevision(std::move(*prior_claim_), prior_claim_revision_),
                                       *prior_proposal_});
    stream_digest_.emplace(stream_hash_.finish());
    return StoredBoundedWriteReceipt{
        *receipt_id_, revision_, std::move(*target_role_), *before_state_, *after_state_,
        expected_.type, slot_width_, std::move(slot_), *before_slot_hash_, *after_slot_hash_,
        *stored_slot_hash_, *applied_delta_hash_, std::move(evidence_), std::move(prior),
        std::move(claim), *proposal_digest_, publication_, *decision_, *binding_, *binding_receipt_,
        *preview_, authority_,
    };
}

// SWEGCA: user@2026-09-22:60-61
const Digest256& BoundedWriteReceiptParser::stream_digest() const {
    if (!stream_digest_) invalid("not_finished");
    return *stream_digest_;
}

}  // namespace swegca::vrs
