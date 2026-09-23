#pragma once

#include "swegca_vrs/allocation.hpp"
#include "swegca_vrs/cognition.hpp"
#include "swegca_vrs/core_sha256.hpp"
#include "swegca_vrs/identity_types.hpp"
#include "swegca_vrs/journal_format.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

// The native byte form of one complete autonomy event, its streaming reader
// and its self-checking writer. The author keeps an event as an in-memory value (AutonomyEvent,
// mosaic_autonomous_cognition.py@5901a5a:89-120) and normalizes only its
// payload, through a JSON round trip (:114-120); a byte form of the whole
// event, its tags and its order are native. The reader establishes that the
// fields a Main route hands to the kernel are the reading of one stream of
// bytes. It does not decide record kind, lineage, how the event's context
// relates to a record's own context, or any claim mapping, and it writes
// nothing. It reads bytes as they are fed; a caller reading an experience
// record's blob must let the record's chunk visit return (the whole-blob
// digest is checked after the last chunk) before it calls `finish`.
//
// Layout, little-endian; every length and count is a u64:
//   magic "SWGAEVNT", u16 version 1, u8 kind (AutonomyEventKind 1-10),
//   text event_id, u8 hypothesis flag [text hypothesis_id],
//   reference count and one text per reference, text source_family,
//   text context_hash, u64 confidence (binary64 bits),
//   payload length, then the payload value, which ends the stream.
// A text is a length and its bytes. event_id, hypothesis_id, the references
// and source_family keep the native identity rule (validate_autonomy_event).
// context_hash is any generalized UTF-8 text that Python's str.strip leaves
// nonempty; it is not kept, only digested (`context_digest`).
//
// A payload value is one u8 tag, then:
//   0 null, 1 false, 2 true;
//   3 integer: u8 sign (1 negative), magnitude length, big-endian magnitude
//     with no leading zero byte (zero: sign 0, length 0);
//   4 float: u64 binary64 bits, a NaN only as 0x7ff8000000000000;
//   5 string: length, then generalized UTF-8 (every code point up to
//     U+10FFFF, surrogates included, in its shortest form);
//   6 array: count, then that many values;
//   7 object: count, then (key, value) pairs; a key is a string without its
//     tag, and keys strictly increase in byte order, which is code point
//     order for this UTF-8.
// The payload is an object. One logical payload has exactly one byte form,
// and `AutonomyPayloadView::digest` is the SHA-256 of those payload bytes.
namespace swegca::vrs {

inline constexpr std::string_view autonomy_event_magic = "SWGAEVNT";
inline constexpr std::uint16_t autonomy_event_version = 1;

// One event read from its bytes, owned on the parser's account. Every text
// is held in its own heap buffer, so the views `view()` returns stay valid
// when this object is moved.
class ParsedAutonomyEvent final {
public:
    ParsedAutonomyEvent(ParsedAutonomyEvent&&) noexcept = default;
    ParsedAutonomyEvent& operator=(ParsedAutonomyEvent&&) = delete;
    ParsedAutonomyEvent(const ParsedAutonomyEvent&) = delete;
    ParsedAutonomyEvent& operator=(const ParsedAutonomyEvent&) = delete;
    ~ParsedAutonomyEvent() = default;

    // The event as the kernel reads it: the envelope fields, the six payload
    // keys it reads (absent, of another type, or present) and the payload
    // digest. It borrows this object, which must outlive it.
    // SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:89-120
    [[nodiscard]] AutonomyEventView view() const;
    // SHA-256 of the domain field `swegca.autonomy_event.context.v1` and the
    // exact context_hash text, each as a u64 length and its bytes. It is the
    // view's context: neither a hex parse nor an experience record's context.
    // SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:101-107
    [[nodiscard]] const Digest256& context_digest() const noexcept { return context_; }

private:
    friend class AutonomyEventParser;
    using Text = journal::LedgerVector<char>;
    using Texts = journal::LedgerVector<Text>;
    using Views = journal::LedgerVector<std::string_view>;

    // SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:89-120
    explicit ParsedAutonomyEvent(const AllocationContext& memory);

    AutonomyEventKind kind_ = AutonomyEventKind::observation;
    Text event_id_;
    std::optional<Text> hypothesis_;
    Texts references_;
    Views reference_views_;
    Text source_family_;
    Digest256 context_{DigestBytes{}};
    double confidence_ = 0;
    // The six keys the kernel reads; the string values are kept below.
    AutonomyPayloadView payload_;
    Texts axes_;
    Views axis_views_;
    Text action_;
    Text memory_ref_;
    Text content_hash_;
};

// Reads one event from bytes fed in order, in any chunking. Its working
// memory is its nesting depth and the object keys on the open path, besides
// what the event keeps; every other payload byte is checked, hashed and
// released. A malformed byte throws `autonomy_event_encoding_invalid:<rule>`;
// an envelope field the author rejects throws `autonomy_event_invalid:<field>`.
// A kept value has no length or count cap of its own; the account's
// allocation is its limit, and nothing is truncated.
class AutonomyEventParser final {
public:
    // SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:89-120
    explicit AutonomyEventParser(const AllocationContext& memory);
    AutonomyEventParser(const AutonomyEventParser&) = delete;
    AutonomyEventParser& operator=(const AutonomyEventParser&) = delete;
    AutonomyEventParser(AutonomyEventParser&&) = delete;
    AutonomyEventParser& operator=(AutonomyEventParser&&) = delete;
    ~AutonomyEventParser() = default;

    // Reads the next bytes of the stream.
    // SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:89-120
    void feed(std::span<const std::byte> bytes);
    // Ends the stream: exactly one complete event must have been fed. Checks
    // the envelope as validate_autonomy_event does and returns the event; the
    // parser is spent afterwards. A failed finish is terminal too.
    // SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:89-120
    [[nodiscard]] ParsedAutonomyEvent finish();

private:
    enum class Step : std::uint8_t {
        magic, version, kind, text_length, text_bytes, hypothesis_flag,
        reference_count, confidence, payload_length, tag, integer_sign,
        integer_length, integer_bytes, float_bits, string_length,
        string_bytes, container_count, key_length, key_bytes, done, spent,
    };
    enum class Field : std::uint8_t { event_id, hypothesis_id, evidence_ref, source_family, context_hash };
    // The payload key the next value belongs to, when the kernel reads it.
    enum class Target : std::uint8_t {
        none, requested_axes, axis, collect_with_tool, action, success, memory_ref, content_hash,
    };
    using Bytes = journal::LedgerVector<std::byte>;

    // One open array or object of the payload.
    struct Frame {
        // SWEGCA: user@2026-09-22:60-61
        explicit Frame(const AllocationContext& memory)
            : previous_key(memory.allocator<std::byte>()), key(memory.allocator<std::byte>()) {}
        bool object = false;
        bool axes = false;  // the requested_axes array
        std::uint64_t remaining = 0;
        bool has_previous_key = false;
        Bytes previous_key;
        Bytes key;
        Target member_target = Target::none;  // root object only
    };

    using Utf8 = detail::GeneralizedUtf8State;

    void consume(std::byte value);
    void on_fixed();
    void begin_text(std::uint64_t length);
    void text_byte(std::byte value);
    void end_text();
    void next_reference();
    void on_tag(std::uint8_t tag);
    void begin_string(std::uint64_t length);
    void begin_container(std::uint64_t count);
    void end_key();
    void value_done();
    [[nodiscard]] Target next_target() const noexcept;
    [[nodiscard]] static Target target_of(std::span<const std::byte> key) noexcept;
    [[nodiscard]] static std::string_view field_name(Field field) noexcept;
    [[nodiscard]] static std::size_t fixed_width(Step step) noexcept;
    // True when a code point completed; throws on an invalid sequence.
    [[nodiscard]] static bool utf8_push(Utf8& state, std::byte value);
    [[nodiscard]] std::uint64_t fixed_u64() const noexcept;

    AllocationContext memory_;
    std::optional<ParsedAutonomyEvent> out_;
    Step step_ = Step::magic;
    bool broken_ = false;  // feed or finish failed; this parser cannot resume
    std::array<std::byte, 8> fixed_{};
    std::size_t fixed_have_ = 0;
    std::uint64_t run_left_ = 0;
    Field field_ = Field::event_id;
    ParsedAutonomyEvent::Text* text_ = nullptr;  // the envelope text being read
    std::uint64_t references_left_ = 0;
    Utf8 utf8_;
    bool context_has_content_ = false;
    Sha256 context_hash_;
    bool in_payload_ = false;
    bool root_started_ = false;
    std::uint64_t payload_left_ = 0;
    Sha256 payload_hash_;
    journal::LedgerVector<Frame> frames_;
    // The buffer of the kept string being read, if any.
    ParsedAutonomyEvent::Text* keep_ = nullptr;
    std::uint8_t pending_tag_ = 0;
    bool pending_axes_ = false;
    bool integer_negative_ = false;
    bool integer_first_ = false;
};

// One value of a complete normalized payload, in pre-order: a container is
// followed by its members, and each object member by a key and its value.
// The payload is what the author's JSON round trip leaves (:114-120).
struct AutonomyPayloadToken {
    enum class Kind : std::uint8_t {
        null_value = 0, false_value = 1, true_value = 2, integer = 3,
        real = 4, string = 5, array = 6, object = 7, key = 8,
    };
    Kind kind = Kind::null_value;
    bool negative = false;             // an integer below zero
    std::span<const std::byte> bytes;  // integer magnitude (big-endian); string or key (generalized UTF-8)
    std::uint64_t count = 0;           // array or object members
    double real = 0;
};

// A complete event as its producer holds it, borrowed for one encoding.
struct AutonomyEventInput {
    AutonomyEventKind kind = AutonomyEventKind::observation;
    std::string_view event_id;
    std::optional<std::string_view> hypothesis_id;
    std::span<const std::string_view> evidence_refs;
    std::string_view source_family;
    std::string_view context_hash;  // generalized UTF-8
    double confidence = 0;
    std::span<const AutonomyPayloadToken> payload;  // pre-order, an object first
};

// Writes the event's native bytes on `memory`, then reads them back with
// AutonomyEventParser, so it returns only bytes that parser accepts. Tokens
// out of shape (a key where a value belongs, a count its members do not fill,
// anything after the payload) throw `autonomy_event_tokens_invalid:<rule>`;
// keys out of order, invalid UTF-8 or a field the author rejects throw as the
// parser does. It writes minimal integer magnitudes (zero is never negative)
// and one native F2 NaN pattern. It does not sort keys: the payload must
// already be in F2 code-point order. It never encodes from
// an AutonomyEventView, whose six projected keys cannot give back the rest.
// SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:89-120
[[nodiscard]] journal::LedgerBytes encode_autonomy_event(const AllocationContext& memory,
                                                         const AutonomyEventInput& event);

}  // namespace swegca::vrs
