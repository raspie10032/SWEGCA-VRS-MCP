#pragma once

#include "swegca_vrs/allocation.hpp"
#include "swegca_vrs/authority.hpp"
#include "swegca_vrs/bounded_write_digests.hpp"
#include "swegca_vrs/cognitive_state.hpp"
#include "swegca_vrs/core_sha256.hpp"
#include "swegca_vrs/identity_types.hpp"
#include "swegca_vrs/journal_position.hpp"
#include "swegca_vrs/main_state_writer.hpp"
#include "swegca_vrs/native_tensor.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

// The native byte stream of one BoundedWriteReceipt, its streaming writer and
// its streaming reader. The author keeps the receipt as a JSON object
// (bounded_world_write_receipt_to_dict/from_dict,
// mosaic_bounded_world_write.py@3bddcb7:243-306); these bytes, their order
// and the C++ audit fields are native. The stream is logical: where its bytes
// are stored, in one record or in parts, how they are published, and what
// authority a receipt may later support are not decided here.
//
// Layout, little-endian; every length and count is a u64, a digest is 32
// bytes and a text is a length and its bytes:
//   magic "SWBWRCPT", u16 version 1,
//   receipt_id, revision, text target_role, before_state_hash,
//   after_state_hash, u8 before_slot_type (ScalarType 1-4), slot_width,
//   before_slot_hash, after_slot_hash, stored_slot_hash, applied_delta_hash,
//   evidence count and one text per reference (proposal order, repeats kept),
//   u8 prior-head flag [text policy_version, receipt_id, revision,
//     text target_role, evidence count and texts, text claim id,
//     claim revision, proposal_digest],
//   text claim id, claim revision, proposal_digest,
//   before_publication: content digest, segment_ordinal, byte_offset,
//     sequence, record_digest,
//   decision_digest, binding_digest, binding_receipt, preview_receipt,
//   u8 authority_domain,
//   before-slot byte length, then the slot bytes, which end the stream.
// Every text keeps the identity rule, so it is at most
// `detail::identity_text_max_bytes`. The slot comes last and is the only
// field whose size is not fixed by that rule or by a count of such texts.
namespace swegca::vrs {

inline constexpr std::string_view bounded_write_receipt_magic = "SWBWRCPT";
inline constexpr std::uint16_t bounded_write_receipt_version = 1;

// The slot shape Main expects, read from its own state's scratch tensor
// (a write never changes that shape). It is the one bound on the slot bytes.
struct ReceiptSlotShape {
    ScalarType type = ScalarType::float32;
    std::uint64_t width = 0;
};

// The publication a receipt was written against, as recorded. It is data:
// only Main may compare it with the chain it verified and construct a
// PublishedStateId from it.
struct StoredStatePublication {
    DigestBytes content_digest{};
    journal::RecordPosition publication;
};

// Every BoundedWriteReceipt field, read back from its stream and owned on the
// reader's account. It differs from BoundedWriteReceipt only in
// `before_publication`, which stays recorded data (see above).
struct StoredBoundedWriteReceipt final {
    using Bytes = BoundedWriteReceipt::Bytes;

    Digest256 receipt_id;
    std::uint64_t revision;
    RoleId target_role;
    Digest256 before_state_hash;
    Digest256 after_state_hash;
    ScalarType before_slot_type;
    std::uint64_t slot_width;
    Bytes before_slot;
    Digest256 before_slot_hash;
    Digest256 after_slot_hash;
    Digest256 stored_slot_hash;
    Digest256 applied_delta_hash;
    EvidenceReferences evidence_references;
    std::optional<BoundedWriteHead> prior_write_head;
    ClaimRevision claim;
    Digest256 proposal_digest;
    StoredStatePublication before_publication;
    Digest256 decision_digest;
    Digest256 binding_digest;
    Digest256 binding_receipt;
    Digest256 preview_receipt;
    AuthorityDomain authority_domain;
};

// Writes the receipt's complete stream to `sink`, in pieces. Small fields go
// through fixed stack buffers and the slot as a span of the receipt's own
// bytes, so nothing proportional to the receipt is allocated. It first checks
// the slot length for its type and width, the slot digest, the receipt id and
// the authority domain; the recorded publication is nonzero already, since
// only a valid PublishedStateId can be constructed. So it either writes a
// stream the reader accepts or throws `bounded_write_receipt_invalid:<rule>`
// before writing any byte.
// SWEGCA: src/tinylm_slicer/mosaic_bounded_world_write.py@3bddcb7:243-270
void encode_bounded_write_receipt(const BoundedWriteReceipt& receipt, StateContentSink sink);

// Reads one receipt from its bytes fed in order, in any chunking. Its working
// memory is fixed besides the text being read (at most the identity bound)
// and what the receipt keeps. The slot's type and width must equal
// `expected` before its length is read; the slot buffer is then sized from
// that checked length and hashed as its bytes arrive. A malformed byte or a
// failed check throws `bounded_write_receipt_invalid:<rule>`, and the reader
// stays failed after any throw.
class BoundedWriteReceiptParser final {
public:
    // SWEGCA: src/tinylm_slicer/mosaic_bounded_world_write.py@3bddcb7:273-306
    BoundedWriteReceiptParser(const AllocationContext& memory, ReceiptSlotShape expected);
    BoundedWriteReceiptParser(const BoundedWriteReceiptParser&) = delete;
    BoundedWriteReceiptParser& operator=(const BoundedWriteReceiptParser&) = delete;
    BoundedWriteReceiptParser(BoundedWriteReceiptParser&&) = delete;
    BoundedWriteReceiptParser& operator=(BoundedWriteReceiptParser&&) = delete;
    ~BoundedWriteReceiptParser() = default;

    // Reads the next bytes of the stream.
    // SWEGCA: src/tinylm_slicer/mosaic_bounded_world_write.py@3bddcb7:273-306
    void feed(std::span<const std::byte> bytes);
    // Ends the stream: exactly one complete receipt must have been fed. Checks
    // the slot digest and the receipt id and returns the receipt; the reader
    // is spent afterwards, and a failed finish is terminal too.
    // SWEGCA: src/tinylm_slicer/mosaic_bounded_world_write.py@3bddcb7:273-306
    [[nodiscard]] StoredBoundedWriteReceipt finish();
    // SHA-256 of every byte fed, available once `finish` has returned.
    // SWEGCA: user@2026-09-22:60-61
    [[nodiscard]] const Digest256& stream_digest() const;

private:
    // The field the next bytes belong to, in stream order.
    enum class Field : std::uint8_t {
        magic, version, receipt_id, revision, target_role, before_state, after_state,
        slot_type, slot_width, before_slot_hash, after_slot_hash, stored_slot_hash,
        applied_delta_hash, evidence_count, evidence, prior_flag, prior_policy,
        prior_receipt_id, prior_revision, prior_role, prior_evidence_count, prior_evidence,
        prior_claim, prior_claim_revision, prior_proposal, claim, claim_revision,
        proposal_digest, publication_content, publication_segment, publication_offset,
        publication_sequence, publication_record, decision, binding, binding_receipt,
        preview, authority, slot_length, slot_bytes, done, spent,
    };
    using Text = std::basic_string<char, std::char_traits<char>, AllocationAdapter<char>>;

    void consume(std::span<const std::byte>& bytes);
    void on_fixed();
    void on_text();
    void begin_field(Field field);
    void after_evidence_count(std::uint64_t count, bool prior);
    [[nodiscard]] static bool is_text(Field field) noexcept;
    [[nodiscard]] static std::size_t fixed_width(Field field) noexcept;
    [[nodiscard]] std::uint64_t fixed_u64() const noexcept;
    [[nodiscard]] Digest256 fixed_digest() const;

    AllocationContext memory_;
    ReceiptSlotShape expected_;
    std::uint64_t expected_slot_bytes_ = 0;
    Field field_ = Field::magic;
    bool broken_ = false;  // a throw left the stream mid-field
    bool text_length_ = true;  // a text field: its length is still being read
    std::array<std::byte, 32> fixed_{};
    std::size_t fixed_have_ = 0;
    std::uint64_t text_left_ = 0;
    Text text_;
    std::uint64_t evidence_left_ = 0;
    std::uint64_t slot_left_ = 0;
    Sha256 stream_hash_;
    std::optional<Digest256> stream_digest_;
    std::optional<detail::SlotDigest> slot_hash_;

    // The fields read so far.
    std::optional<Digest256> receipt_id_;
    std::uint64_t revision_ = 0;
    std::optional<RoleId> target_role_;
    std::optional<Digest256> before_state_;
    std::optional<Digest256> after_state_;
    std::uint64_t slot_width_ = 0;
    std::optional<Digest256> before_slot_hash_;
    std::optional<Digest256> after_slot_hash_;
    std::optional<Digest256> stored_slot_hash_;
    std::optional<Digest256> applied_delta_hash_;
    EvidenceReferences evidence_;
    bool has_prior_ = false;
    std::optional<PolicyVersion> prior_policy_;
    std::optional<Digest256> prior_receipt_id_;
    std::uint64_t prior_revision_ = 0;
    std::optional<RoleId> prior_role_;
    EvidenceReferences prior_evidence_;
    std::optional<ClaimId> prior_claim_;
    std::uint64_t prior_claim_revision_ = 0;
    std::optional<Digest256> prior_proposal_;
    std::optional<ClaimId> claim_;
    std::uint64_t claim_revision_ = 0;
    std::optional<Digest256> proposal_digest_;
    StoredStatePublication publication_;
    std::optional<Digest256> decision_;
    std::optional<Digest256> binding_;
    std::optional<Digest256> binding_receipt_;
    std::optional<Digest256> preview_;
    AuthorityDomain authority_ = AuthorityDomain::cognitive_state_commit;
    StoredBoundedWriteReceipt::Bytes slot_;
};

}  // namespace swegca::vrs
