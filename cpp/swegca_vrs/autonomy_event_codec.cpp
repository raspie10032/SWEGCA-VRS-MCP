#include "swegca_vrs/autonomy_event_codec.hpp"

#include <algorithm>
#include <bit>
#include <stdexcept>
#include <string>
#include <utility>

namespace swegca::vrs {
namespace {

constexpr std::string_view context_domain = "swegca.autonomy_event.context.v1";

// SWEGCA: user@2026-09-22:60-61
[[noreturn]] void fail(const std::string& code) { throw std::invalid_argument(code); }

// Lineage: native mechanism — a byte that breaks the native layout.
// SWEGCA: user@2026-09-22:60-61
[[noreturn]] void malformed(std::string_view rule) {
    fail("autonomy_event_encoding_invalid:" + std::string(rule));
}

// A field the author's AutonomyEvent.__post_init__ rejects, named as
// validate_autonomy_event names it.
// SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:100-120
[[noreturn]] void invalid(std::string_view field) {
    fail("autonomy_event_invalid:" + std::string(field));
}

// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:376-388
void hash_u64(Sha256& hash, std::uint64_t value) {
    std::array<std::byte, 8> bytes{};
    for (std::size_t at = 0; at < bytes.size(); ++at)
        bytes[at] = static_cast<std::byte>((value >> (8 * at)) & 0xff);
    hash.update(bytes);
}

// SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:89-120
std::string_view text_view(const journal::LedgerVector<char>& text) noexcept {
    return std::string_view(text.data(), text.size());
}

// The author's isinstance test on one value the kernel reads: present when
// the tag is the expected type, otherwise of another type.
// SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:267-380
template <class T>
void mark(PayloadField<T>& field, bool expected_type) noexcept {
    field.state = expected_type ? PayloadField<T>::State::present : PayloadField<T>::State::wrong_type;
}

constexpr std::array<std::string_view, 5> field_names{
    "event_id", "hypothesis_id", "evidence_refs", "source_family", "context_hash"};

constexpr std::uint64_t canonical_nan_bits = 0x7ff8000000000000ull;

// Lineage: native mechanism — a token list that cannot be the payload.
// SWEGCA: user@2026-09-22:60-61
[[noreturn]] void bad_tokens(std::string_view rule) {
    fail("autonomy_event_tokens_invalid:" + std::string(rule));
}

// SWEGCA: user@2026-09-22:60-61
void append_raw(journal::LedgerBytes& out, std::span<const std::byte> bytes) {
    out.insert(out.end(), bytes.begin(), bytes.end());
}

// Little-endian, `width` bytes.
// SWEGCA: user@2026-09-22:60-61
void append_uint(journal::LedgerBytes& out, std::uint64_t value, std::size_t width) {
    for (std::size_t at = 0; at < width; ++at)
        out.push_back(static_cast<std::byte>((value >> (8 * at)) & 0xff));
}

// SWEGCA: user@2026-09-22:60-61
void append_text(journal::LedgerBytes& out, std::string_view text) {
    append_uint(out, text.size(), 8);
    append_raw(out, std::as_bytes(std::span<const char>(text.data(), text.size())));
}

// Writes the payload tokens, checking only their shape; the parser that
// reads the result back checks everything else.
// Lineage: native mechanism — pre-order tokens to the layout's tags.
// SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:114-120
void append_payload(journal::LedgerBytes& out, const AllocationContext& memory,
                    std::span<const AutonomyPayloadToken> tokens) {
    using Kind = AutonomyPayloadToken::Kind;
    struct Open {
        bool object = false;
        std::uint64_t remaining = 0;
        bool key_next = false;
    };
    journal::LedgerVector<Open> open(memory.allocator<Open>());
    if (tokens.empty() || tokens.front().kind != Kind::object) bad_tokens("payload_not_object");
    bool root_done = false;
    for (const auto& token : tokens) {
        if (root_done) bad_tokens("trailing_tokens");
        const bool key_expected = !open.empty() && open.back().object && open.back().key_next;
        if (key_expected != (token.kind == Kind::key)) bad_tokens("key_position");
        if (token.kind == Kind::key) {
            append_uint(out, token.bytes.size(), 8);
            append_raw(out, token.bytes);
            open.back().key_next = false;
            continue;
        }
        append_uint(out, static_cast<std::uint8_t>(token.kind), 1);
        switch (token.kind) {
        case Kind::null_value:
        case Kind::false_value:
        case Kind::true_value:
            break;
        case Kind::integer: {
            auto magnitude = token.bytes;
            while (!magnitude.empty() && magnitude.front() == std::byte{0}) magnitude = magnitude.subspan(1);
            append_uint(out, token.negative && !magnitude.empty() ? 1 : 0, 1);
            append_uint(out, magnitude.size(), 8);
            append_raw(out, magnitude);
            break;
        }
        case Kind::real: {
            auto bits = std::bit_cast<std::uint64_t>(token.real);
            if ((bits & 0x7ff0000000000000ull) == 0x7ff0000000000000ull && (bits & 0x000fffffffffffffull) != 0)
                bits = canonical_nan_bits;
            append_uint(out, bits, 8);
            break;
        }
        case Kind::string:
            append_uint(out, token.bytes.size(), 8);
            append_raw(out, token.bytes);
            break;
        case Kind::array:
        case Kind::object:
            append_uint(out, token.count, 8);
            if (token.count != 0) {
                const bool object = token.kind == Kind::object;
                open.push_back(Open{object, token.count, object});
                continue;
            }
            break;
        default:
            bad_tokens("kind");
        }
        // A value completed: close every container it filled.
        while (true) {
            if (open.empty()) {
                root_done = true;
                break;
            }
            auto& top = open.back();
            if (--top.remaining != 0) {
                top.key_next = top.object;
                break;
            }
            open.pop_back();
        }
    }
    if (!root_done) bad_tokens("truncated");
}

}  // namespace

// SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:89-120
ParsedAutonomyEvent::ParsedAutonomyEvent(const AllocationContext& memory)
    : event_id_(memory.allocator<char>()), references_(memory.allocator<Text>()),
      reference_views_(memory.allocator<std::string_view>()),
      source_family_(memory.allocator<char>()), axes_(memory.allocator<Text>()),
      axis_views_(memory.allocator<std::string_view>()), action_(memory.allocator<char>()),
      memory_ref_(memory.allocator<char>()), content_hash_(memory.allocator<char>()) {}

// SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:89-120
AutonomyEventView ParsedAutonomyEvent::view() const {
    AutonomyPayloadView payload = payload_;
    payload.requested_axes.value = std::span<const std::string_view>(axis_views_.data(), axis_views_.size());
    payload.action.value = text_view(action_);
    payload.memory_ref.value = text_view(memory_ref_);
    payload.content_hash.value = text_view(content_hash_);
    std::optional<std::string_view> hypothesis;
    if (hypothesis_) hypothesis = text_view(*hypothesis_);
    return AutonomyEventView{
        .event_id = text_view(event_id_),
        .kind = kind_,
        .hypothesis_id = hypothesis,
        .evidence_refs = std::span<const std::string_view>(reference_views_.data(), reference_views_.size()),
        .source_family = text_view(source_family_),
        .context = context_,
        .confidence = confidence_,
        .payload = payload,
    };
}

// The context digest starts with its domain field; the text's length and
// bytes follow as they are read.
// Lineage: native mechanism — the author's context_hash is a plain text (:96); this digest and its domain are C++'s.
// SWEGCA: user@2026-09-22:60-61
AutonomyEventParser::AutonomyEventParser(const AllocationContext& memory)
    : memory_(memory), frames_(memory.allocator<Frame>()) {
    out_.emplace(ParsedAutonomyEvent(memory));
    hash_u64(context_hash_, context_domain.size());
    context_hash_.update(context_domain);
}

// Only payload bytes enter the payload digest: those after the payload
// length, up to the end of the payload value.
// SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:114-120
void AutonomyEventParser::feed(std::span<const std::byte> bytes) {
    if (broken_) fail("autonomy_event_parser_failed");
    if (step_ == Step::spent) fail("autonomy_event_parser_spent");
    broken_ = true;
    bool hashing = in_payload_;
    std::size_t hash_from = 0;
    for (std::size_t at = 0; at < bytes.size(); ++at) {
        const bool was_payload = in_payload_;
        if (was_payload) {
            if (payload_left_ == 0) malformed("payload_length");
            --payload_left_;
        }
        consume(bytes[at]);
        if (!was_payload && in_payload_) {
            hashing = true;
            hash_from = at + 1;
        } else if (was_payload && !in_payload_) {
            payload_hash_.update(bytes.subspan(hash_from, at + 1 - hash_from));
            hashing = false;
        }
    }
    if (hashing) payload_hash_.update(bytes.subspan(hash_from));
    broken_ = false;
}

// SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:89-120
ParsedAutonomyEvent AutonomyEventParser::finish() {
    if (broken_) fail("autonomy_event_parser_failed");
    if (step_ == Step::spent) fail("autonomy_event_parser_spent");
    broken_ = true;
    if (step_ != Step::done) malformed("truncated");
    auto& out = *out_;
    out.payload_.digest = payload_hash_.finish();
    for (const auto& reference : out.references_) out.reference_views_.push_back(text_view(reference));
    using AxesState = PayloadField<std::span<const std::string_view>>::State;
    if (out.payload_.requested_axes.state == AxesState::present)
        for (const auto& axis : out.axes_) out.axis_views_.push_back(text_view(axis));
    validate_autonomy_event(out.view());
    step_ = Step::spent;
    ParsedAutonomyEvent result(std::move(out));
    out_.reset();
    broken_ = false;
    return result;
}

// Lineage: native mechanism — one byte of the layout at a time; texts,
// strings, keys and magnitudes stream, fixed-width fields are gathered.
// SWEGCA: user@2026-09-22:60-61
void AutonomyEventParser::consume(std::byte value) {
    switch (step_) {
    case Step::text_bytes:
        text_byte(value);
        return;
    case Step::integer_bytes:
        if (integer_first_ && value == std::byte{0}) malformed("integer_leading_zero");
        integer_first_ = false;
        if (--run_left_ == 0) value_done();
        return;
    case Step::string_bytes:
        (void)utf8_push(utf8_, value);
        if (keep_) keep_->push_back(static_cast<char>(value));
        if (--run_left_ == 0) {
            if (utf8_.pending != 0) malformed("string_utf8");
            keep_ = nullptr;
            value_done();
        }
        return;
    case Step::key_bytes:
        (void)utf8_push(utf8_, value);
        frames_.back().key.push_back(value);
        if (--run_left_ == 0) {
            if (utf8_.pending != 0) malformed("key_utf8");
            end_key();
        }
        return;
    case Step::done:
        malformed("trailing_bytes");
    case Step::spent:
        fail("autonomy_event_parser_spent");
    default:
        break;
    }
    fixed_[fixed_have_++] = value;
    if (fixed_have_ < fixed_width(step_)) return;
    fixed_have_ = 0;
    on_fixed();
}

// SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:89-120
void AutonomyEventParser::on_fixed() {
    auto& out = *out_;
    const auto value = fixed_u64();
    switch (step_) {
    case Step::magic:
        if (!std::equal(fixed_.begin(), fixed_.end(), autonomy_event_magic.begin(),
                        [](std::byte byte, char expected) { return byte == static_cast<std::byte>(expected); }))
            malformed("magic");
        step_ = Step::version;
        return;
    case Step::version:
        if (value != autonomy_event_version) malformed("version");
        step_ = Step::kind;
        return;
    case Step::kind:
        if (value < 1 || value > 10) invalid("kind");
        out.kind_ = static_cast<AutonomyEventKind>(value);
        field_ = Field::event_id;
        step_ = Step::text_length;
        return;
    case Step::text_length:
        begin_text(value);
        return;
    case Step::hypothesis_flag:
        if (value > 1) malformed("hypothesis_flag");
        if (value == 1) {
            out.hypothesis_.emplace(memory_.allocator<char>());
            field_ = Field::hypothesis_id;
            step_ = Step::text_length;
        } else {
            step_ = Step::reference_count;
        }
        return;
    case Step::reference_count:
        references_left_ = value;
        next_reference();
        return;
    case Step::confidence:
        out.confidence_ = std::bit_cast<double>(value);
        step_ = Step::payload_length;
        return;
    case Step::payload_length:
        payload_left_ = value;
        in_payload_ = true;
        step_ = Step::tag;
        return;
    case Step::tag:
        on_tag(static_cast<std::uint8_t>(value));
        return;
    case Step::integer_sign:
        if (value > 1) malformed("integer_sign");
        integer_negative_ = value == 1;
        step_ = Step::integer_length;
        return;
    case Step::integer_length:
        if (value == 0) {
            if (integer_negative_) malformed("integer_negative_zero");
            value_done();
            return;
        }
        run_left_ = value;
        integer_first_ = true;
        step_ = Step::integer_bytes;
        return;
    case Step::float_bits:
        // F2 uses one native NaN bit pattern. Signed zero and infinities keep
        // their binary64 bits; no CPython NaN representation is assumed.
        if ((value & 0x7ff0000000000000ull) == 0x7ff0000000000000ull &&
            (value & 0x000fffffffffffffull) != 0 && value != canonical_nan_bits)
            malformed("float_nan");
        value_done();
        return;
    case Step::string_length:
        begin_string(value);
        return;
    case Step::container_count:
        begin_container(value);
        return;
    case Step::key_length: {
        auto& frame = frames_.back();
        frame.key.clear();
        utf8_ = Utf8{};
        if (value == 0) {
            end_key();
            return;
        }
        run_left_ = value;
        step_ = Step::key_bytes;
        return;
    }
    default:
        malformed("state");
    }
}

// The four identity texts are held up to the native identity length; the
// context text is only checked and digested, so its length is not bounded.
// SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:100-110
void AutonomyEventParser::begin_text(std::uint64_t length) {
    auto& out = *out_;
    switch (field_) {
    case Field::event_id: text_ = &out.event_id_; break;
    case Field::hypothesis_id: text_ = &*out.hypothesis_; break;
    case Field::evidence_ref: text_ = &out.references_.back(); break;
    case Field::source_family: text_ = &out.source_family_; break;
    case Field::context_hash:
        text_ = nullptr;
        utf8_ = Utf8{};
        context_has_content_ = false;
        hash_u64(context_hash_, length);
        break;
    }
    if (text_ && length > detail::identity_text_max_bytes) invalid(field_name(field_));
    run_left_ = length;
    if (length == 0) {
        end_text();
        return;
    }
    step_ = Step::text_bytes;
}

// SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:100-110
void AutonomyEventParser::text_byte(std::byte value) {
    if (text_) {
        text_->push_back(static_cast<char>(value));
    } else {
        if (utf8_push(utf8_, value) && !detail::is_python_strip_space(utf8_.code_point))
            context_has_content_ = true;
        context_hash_.update(std::span<const std::byte>(&value, 1));
    }
    if (--run_left_ == 0) end_text();
}

// The context text must hold something str.strip keeps (the author's
// `not value.strip()` check, :101-107).
// SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:100-110
void AutonomyEventParser::end_text() {
    text_ = nullptr;
    switch (field_) {
    case Field::event_id:
        step_ = Step::hypothesis_flag;
        return;
    case Field::hypothesis_id:
        step_ = Step::reference_count;
        return;
    case Field::evidence_ref:
        next_reference();
        return;
    case Field::source_family:
        field_ = Field::context_hash;
        step_ = Step::text_length;
        return;
    case Field::context_hash:
        if (utf8_.pending != 0) malformed("context_utf8");
        if (!context_has_content_) invalid("context_hash");
        out_->context_ = Digest256(context_hash_.finish());
        step_ = Step::confidence;
        return;
    }
}

// Each reference is held as its bytes arrive, never reserved from a count.
// SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:94-110
void AutonomyEventParser::next_reference() {
    if (references_left_ == 0) {
        field_ = Field::source_family;
        step_ = Step::text_length;
        return;
    }
    --references_left_;
    out_->references_.emplace_back(memory_.allocator<char>());
    field_ = Field::evidence_ref;
    step_ = Step::text_length;
}

// Projects the value the kernel reads with `.get()` from the payload's top
// level (:267-380); nested keys and every other key are only checked and
// hashed.
// SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:267-380
void AutonomyEventParser::on_tag(std::uint8_t tag) {
    const auto target = next_target();
    if (frames_.empty()) {
        if (root_started_) malformed("state");
        // The author's payload is a mapping (:114-120).
        if (tag != 7) malformed("payload_not_object");
        root_started_ = true;
    }
    auto& out = *out_;
    auto& payload = out.payload_;
    keep_ = nullptr;
    switch (target) {
    case Target::none:
        break;
    case Target::collect_with_tool:
        mark(payload.collect_with_tool, tag == 1 || tag == 2);
        payload.collect_with_tool.value = tag == 2;
        break;
    case Target::success:
        mark(payload.success, tag == 1 || tag == 2);
        payload.success.value = tag == 2;
        break;
    case Target::action:
        mark(payload.action, tag == 5);
        if (tag == 5) keep_ = &out.action_;
        break;
    case Target::memory_ref:
        mark(payload.memory_ref, tag == 5);
        if (tag == 5) keep_ = &out.memory_ref_;
        break;
    case Target::content_hash:
        mark(payload.content_hash, tag == 5);
        if (tag == 5) keep_ = &out.content_hash_;
        break;
    case Target::requested_axes:
        mark(payload.requested_axes, tag == 6);
        break;
    case Target::axis:
        // One element that is not a string makes the whole list of another
        // type (:268-270); the axes kept so far are released.
        if (tag == 5) {
            keep_ = &out.axes_.emplace_back(memory_.allocator<char>());
        } else {
            mark(payload.requested_axes, false);
            out.axes_.clear();
        }
        break;
    }
    switch (tag) {
    case 0:
    case 1:
    case 2:
        value_done();
        return;
    case 3:
        step_ = Step::integer_sign;
        return;
    case 4:
        step_ = Step::float_bits;
        return;
    case 5:
        step_ = Step::string_length;
        return;
    case 6:
    case 7:
        pending_tag_ = tag;
        pending_axes_ = target == Target::requested_axes && tag == 6;
        step_ = Step::container_count;
        return;
    default:
        malformed("tag");
    }
}

// A kept string grows as its bytes arrive; the account's allocation is its
// only limit, as for the typed control that stores it.
// SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:267-380
void AutonomyEventParser::begin_string(std::uint64_t length) {
    utf8_ = Utf8{};
    run_left_ = length;
    if (length == 0) {
        keep_ = nullptr;
        value_done();
        return;
    }
    step_ = Step::string_bytes;
}

// Lineage: native mechanism — a container is opened only when it has
// members; its count is checked against the payload length as bytes arrive,
// never reserved.
// SWEGCA: user@2026-09-22:60-61
void AutonomyEventParser::begin_container(std::uint64_t count) {
    if (count == 0) {
        value_done();
        return;
    }
    auto& frame = frames_.emplace_back(memory_);
    frame.object = pending_tag_ == 7;
    frame.axes = pending_axes_;
    frame.remaining = count;
    step_ = frame.object ? Step::key_length : Step::tag;
}

// F2 keys strictly increase by code point, so a key cannot repeat and one
// normalized mapping has one native byte form. This order is the native
// encoding rule; source normalization may convert non-string input keys.
// SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:114-120
void AutonomyEventParser::end_key() {
    auto& frame = frames_.back();
    if (frame.has_previous_key &&
        !std::lexicographical_compare(frame.previous_key.begin(), frame.previous_key.end(),
                                      frame.key.begin(), frame.key.end()))
        malformed("key_order");
    frame.previous_key.swap(frame.key);
    frame.has_previous_key = true;
    if (frames_.size() == 1) frame.member_target = target_of(frame.previous_key);
    step_ = Step::tag;
}

// Lineage: native mechanism — closes every container the value completed;
// the payload value ends the stream at exactly the payload length.
// SWEGCA: user@2026-09-22:60-61
void AutonomyEventParser::value_done() {
    while (true) {
        if (frames_.empty()) {
            if (payload_left_ != 0) malformed("payload_length");
            in_payload_ = false;
            step_ = Step::done;
            return;
        }
        auto& frame = frames_.back();
        if (--frame.remaining != 0) {
            step_ = frame.object ? Step::key_length : Step::tag;
            return;
        }
        frames_.pop_back();
    }
}

// SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:267-380
AutonomyEventParser::Target AutonomyEventParser::next_target() const noexcept {
    if (frames_.empty()) return Target::none;
    const auto& frame = frames_.back();
    if (frame.object) return frames_.size() == 1 ? frame.member_target : Target::none;
    using AxesState = PayloadField<std::span<const std::string_view>>::State;
    return frame.axes && out_->payload_.requested_axes.state == AxesState::present ? Target::axis
                                                                                    : Target::none;
}

// The six keys the kernel reads (:267, :272, :284, :301, :346-347, :360, :374).
// SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:267-380
AutonomyEventParser::Target AutonomyEventParser::target_of(std::span<const std::byte> key) noexcept {
    const std::string_view name(reinterpret_cast<const char*>(key.data()), key.size());
    if (name == "requested_axes") return Target::requested_axes;
    if (name == "collect_with_tool") return Target::collect_with_tool;
    if (name == "action") return Target::action;
    if (name == "success") return Target::success;
    if (name == "memory_ref") return Target::memory_ref;
    if (name == "content_hash") return Target::content_hash;
    return Target::none;
}

// SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:100-120
std::string_view AutonomyEventParser::field_name(Field field) noexcept {
    return field_names[static_cast<std::size_t>(field)];
}

// SWEGCA: user@2026-09-22:60-61
std::size_t AutonomyEventParser::fixed_width(Step step) noexcept {
    switch (step) {
    case Step::magic: return 8;
    case Step::version: return 2;
    case Step::kind:
    case Step::hypothesis_flag:
    case Step::tag:
    case Step::integer_sign: return 1;
    case Step::text_length:
    case Step::reference_count:
    case Step::confidence:
    case Step::payload_length:
    case Step::integer_length:
    case Step::float_bits:
    case Step::string_length:
    case Step::container_count:
    case Step::key_length: return 8;
    default: return 0;
    }
}

// Lineage: native mechanism — the parser and durable control share the same
// generalized UTF-8 rule; a Python str may hold lone surrogate code points.
// SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:114-120
bool AutonomyEventParser::utf8_push(Utf8& state, std::byte value) {
    bool complete = false;
    if (!detail::push_generalized_utf8(state, value, complete)) malformed("utf8");
    return complete;
}

// SWEGCA: user@2026-09-22:60-61
std::uint64_t AutonomyEventParser::fixed_u64() const noexcept {
    std::uint64_t value = 0;
    const auto width = fixed_width(step_);
    for (std::size_t at = 0; at < width; ++at)
        value |= std::to_integer<std::uint64_t>(fixed_[at]) << (8 * at);
    return value;
}

// The payload length is written after the payload; the parser then reads the
// whole result, so what returns is exactly what it accepts.
// Lineage: native mechanism — the envelope in the layout's order.
// SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:89-120
journal::LedgerBytes encode_autonomy_event(const AllocationContext& memory,
                                           const AutonomyEventInput& event) {
    journal::LedgerBytes out(memory.allocator<std::byte>());
    append_raw(out, std::as_bytes(std::span<const char>(autonomy_event_magic.data(), autonomy_event_magic.size())));
    append_uint(out, autonomy_event_version, 2);
    append_uint(out, static_cast<std::uint8_t>(event.kind), 1);
    append_text(out, event.event_id);
    append_uint(out, event.hypothesis_id ? 1 : 0, 1);
    if (event.hypothesis_id) append_text(out, *event.hypothesis_id);
    append_uint(out, event.evidence_refs.size(), 8);
    for (const auto reference : event.evidence_refs) append_text(out, reference);
    append_text(out, event.source_family);
    append_text(out, event.context_hash);
    append_uint(out, std::bit_cast<std::uint64_t>(event.confidence), 8);
    const auto length_at = out.size();
    append_uint(out, 0, 8);
    const auto payload_at = out.size();
    append_payload(out, memory, event.payload);
    const std::uint64_t length = out.size() - payload_at;
    for (std::size_t at = 0; at < 8; ++at)
        out[length_at + at] = static_cast<std::byte>((length >> (8 * at)) & 0xff);
    AutonomyEventParser check(memory);
    check.feed(out);
    (void)check.finish();
    return out;
}

}  // namespace swegca::vrs
