#include "swegca_vrs/autonomy_event_codec.hpp"

#include <algorithm>
#include <bit>
#include <limits>
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
// SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:113-119
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
        // One NaN pattern, assumed to be the one json.loads returns (CPython's
        // float("nan"), not measured here); -0.0 and the
        // infinities are kept as they are.
        if ((value & 0x7ff0000000000000ull) == 0x7ff0000000000000ull &&
            (value & 0x000fffffffffffffull) != 0 && value != 0x7ff8000000000000ull)
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
// `not value.strip()` check, :101-106).
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
        // The author's payload is a mapping (:113-119).
        if (tag != 7) malformed("payload_not_object");
        root_started_ = true;
    }
    auto& out = *out_;
    auto& payload = out.payload_;
    keep_ = nullptr;
    target_ = target;
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

// A kept string the typed control cannot store fails rather than being cut
// (AutonomyControl::encode bounds each text by journal::max_payload_bytes).
// SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:267-380
void AutonomyEventParser::begin_string(std::uint64_t length) {
    if (keep_ && length > journal::max_payload_bytes)
        fail("autonomy_event_unstorable:" + std::string(target_name(target_)));
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
    if (pending_axes_ && count > std::numeric_limits<std::uint32_t>::max())
        fail("autonomy_event_unstorable:requested_axes");
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

// Keys strictly increase, so a key cannot repeat and a mapping has one byte
// form (the author's normalized mapping, :113-119, sorted as json.dumps
// sort_keys orders it by code point).
// SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:113-119
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

// SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:267-380
std::string_view AutonomyEventParser::target_name(Target target) noexcept {
    switch (target) {
    case Target::requested_axes:
    case Target::axis: return "requested_axes";
    case Target::collect_with_tool: return "collect_with_tool";
    case Target::action: return "action";
    case Target::success: return "success";
    case Target::memory_ref: return "memory_ref";
    case Target::content_hash: return "content_hash";
    case Target::none: break;
    }
    return "none";
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

// Lineage: native mechanism — generalized UTF-8: shortest form, at most
// U+10FFFF, surrogate code points allowed, since a Python str may hold them.
// SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:113-119
bool AutonomyEventParser::utf8_push(Utf8& state, std::byte value) {
    const auto byte = std::to_integer<std::uint32_t>(value);
    if (state.pending == 0) {
        if (byte < 0x80) {
            state.code_point = byte;
            return true;
        }
        if (byte >= 0xc2 && byte <= 0xdf) {
            state.code_point = byte & 0x1f;
            state.minimum = 0x80;
            state.pending = 1;
        } else if (byte >= 0xe0 && byte <= 0xef) {
            state.code_point = byte & 0x0f;
            state.minimum = 0x800;
            state.pending = 2;
        } else if (byte >= 0xf0 && byte <= 0xf4) {
            state.code_point = byte & 0x07;
            state.minimum = 0x10000;
            state.pending = 3;
        } else {
            malformed("utf8");
        }
        return false;
    }
    if ((byte & 0xc0) != 0x80) malformed("utf8");
    state.code_point = (state.code_point << 6) | (byte & 0x3f);
    if (--state.pending != 0) return false;
    if (state.code_point < state.minimum || state.code_point > 0x10ffff) malformed("utf8");
    return true;
}

// SWEGCA: user@2026-09-22:60-61
std::uint64_t AutonomyEventParser::fixed_u64() const noexcept {
    std::uint64_t value = 0;
    const auto width = fixed_width(step_);
    for (std::size_t at = 0; at < width; ++at)
        value |= std::to_integer<std::uint64_t>(fixed_[at]) << (8 * at);
    return value;
}

}  // namespace swegca::vrs
