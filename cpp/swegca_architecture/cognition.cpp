#include "swegca_architecture/cognition.hpp"

#include "swegca_architecture/evidence_accumulator.hpp"
#include "swegca_architecture/sha256.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace swegca::architecture {
namespace {

using journal::ByteReader;
using journal::ByteWriter;
using journal::LedgerBytes;
using journal::LedgerVector;

constexpr std::uint16_t control_version = 1;
constexpr auto text_limit = detail::identity_text_max_bytes;
constexpr auto payload_limit = journal::max_payload_bytes;

// SWEGCA: user@2026-09-22:60-61
[[noreturn]] void fail(const std::string& code) { throw std::invalid_argument(code); }

// SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:112-113
bool unit_interval(double value) noexcept { return std::isfinite(value) && value >= 0 && value <= 1; }

// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:376-388
void hash_u64(Sha256& hash, std::uint64_t value) {
    std::array<std::byte, 8> bytes{};
    for (std::size_t at = 0; at < bytes.size(); ++at)
        bytes[at] = static_cast<std::byte>((value >> (8 * at)) & 0xff);
    hash.update(bytes);
}

// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:376-388
void hash_field(Sha256& hash, std::string_view text) {
    hash_u64(hash, text.size());
    hash.update(text);
}

// SWEGCA: src/swegca/mosaic_unrestricted_experience.py@5901a5a:376-388
void hash_optional(Sha256& hash, const std::optional<std::string_view>& text) {
    hash_u64(hash, text ? 1 : 0);
    hash_field(hash, text.value_or(std::string_view()));
}

// Equal as the author's `==` between an optional event text and a stored
// optional value (None == None).
// SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:208-235
template <class Text>
bool same(const std::optional<std::string_view>& event, const std::optional<Text>& stored) noexcept {
    if (!event || !stored) return !event && !stored;
    return *event == stored->value();
}

// A present, nonempty text (the author's `isinstance(value, str) and value`).
// SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:346-356
bool nonempty_text(const PayloadField<std::string_view>& field) noexcept {
    return field.state == PayloadField<std::string_view>::State::present && !field.value.empty();
}

// SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:137-148
AutonomyEventKind expected_event(AutonomyPhase phase) noexcept {
    switch (phase) {
    case AutonomyPhase::observe: return AutonomyEventKind::observation;
    case AutonomyPhase::hypothesize: return AutonomyEventKind::hypothesis;
    case AutonomyPhase::request_evidence: return AutonomyEventKind::evidence_request;
    case AutonomyPhase::collect_evidence: return AutonomyEventKind::evidence_action;
    case AutonomyPhase::observe_evidence_result: return AutonomyEventKind::evidence_result;
    case AutonomyPhase::verify: return AutonomyEventKind::verification;
    case AutonomyPhase::remember: return AutonomyEventKind::memory_commit;
    case AutonomyPhase::act: return AutonomyEventKind::action_proposal;
    case AutonomyPhase::observe_result: return AutonomyEventKind::action_result;
    case AutonomyPhase::abstain: return AutonomyEventKind::observation;
    }
    return AutonomyEventKind::observation;
}

// SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:155-164
AutonomyStep reject(AutonomyPhase phase, AutonomyReason reason) noexcept {
    AutonomyStep out;
    out.accepted = false;
    out.reason = reason;
    out.prior_phase = phase;
    out.next_phase = phase;
    return out;
}

// Sorted, unique actions from the producer's list; each an identity text.
// SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:52-86
LedgerVector<ToolAction> action_set(const AllocationContext& memory,
                                    std::span<const std::string_view> actions) {
    LedgerVector<ToolAction> out(memory.allocator<ToolAction>());
    out.reserve(actions.size());
    for (const auto action : actions) out.emplace_back(memory, action);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

// SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:52-86
bool contains(const LedgerVector<ToolAction>& actions, std::string_view action) noexcept {
    const auto found = std::lower_bound(actions.begin(), actions.end(), action,
                                        [](const ToolAction& item, std::string_view key) { return item.value() < key; });
    return found != actions.end() && found->value() == action;
}

// SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:52-86
bool subset(const LedgerVector<ToolAction>& part, const LedgerVector<ToolAction>& whole) noexcept {
    return std::all_of(part.begin(), part.end(),
                       [&whole](const ToolAction& action) { return contains(whole, action.value()); });
}

// SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:41-86
void hash_actions(Sha256& hash, const LedgerVector<ToolAction>& actions) {
    hash_u64(hash, actions.size());
    for (const auto& action : actions) hash_field(hash, action.value());
}

// SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:167-186
template <class Text>
void write_optional(ByteWriter& writer, const std::optional<Text>& text) {
    writer.u8(text ? 1 : 0);
    if (text) writer.text(text->value(), text_limit);
}

// -0 is kept as +0 so equal values have equal bytes.
// SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:167-186
void write_optional(ByteWriter& writer, const std::optional<double>& value) {
    writer.u8(value ? 1 : 0);
    writer.u64(value ? std::bit_cast<std::uint64_t>(*value + 0.0) : 0);
}

// SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:167-186
template <class Text>
std::optional<Text> read_optional(ByteReader& reader, const AllocationContext& memory) {
    const auto flag = reader.u8();
    if (flag > 1) fail("autonomy_control_invalid");
    if (flag == 0) return std::nullopt;
    return Text(memory, reader.text_view(text_limit));
}

// SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:167-186
std::optional<double> read_optional_double(ByteReader& reader) {
    const auto flag = reader.u8();
    const auto bits = reader.u64();
    if (flag > 1 || (flag == 0 && bits != 0)) fail("autonomy_control_invalid");
    if (flag == 0) return std::nullopt;
    const auto value = std::bit_cast<double>(bits);
    if (!std::isfinite(value) || std::bit_cast<std::uint64_t>(value + 0.0) != bits)
        fail("autonomy_control_invalid");
    return value;
}

// SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:167-186
void write_optional(ByteWriter& writer, const std::optional<PayloadText>& text) {
    writer.u8(text ? 1 : 0);
    if (text) writer.bytes(std::as_bytes(std::span<const char>(text->data(), text->size())), payload_limit);
}

// SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:167-186
PayloadText payload_text(const AllocationContext& memory, std::span<const std::byte> bytes) {
    return PayloadText(reinterpret_cast<const char*>(bytes.data()), bytes.size(), memory.allocator<char>());
}

// SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:167-186
std::optional<PayloadText> read_optional_payload(ByteReader& reader, const AllocationContext& memory) {
    const auto flag = reader.u8();
    if (flag > 1) fail("autonomy_control_invalid");
    if (flag == 0) return std::nullopt;
    return payload_text(memory, reader.bytes_view(payload_limit));
}

}  // namespace

// SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:189-410
std::string_view autonomy_reason_text(AutonomyReason reason) noexcept {
    switch (reason) {
    case AutonomyReason::accepted: return "accepted";
    case AutonomyReason::duplicate_event: return "duplicate_event";
    case AutonomyReason::hypothesis_not_abandonable: return "hypothesis_not_abandonable";
    case AutonomyReason::hypothesis_mismatch: return "hypothesis_mismatch";
    case AutonomyReason::hypothesis_abandoned: return "hypothesis_abandoned";
    case AutonomyReason::unexpected_event_for_phase: return "unexpected_event_for_phase";
    case AutonomyReason::hypothesis_requires_provenance: return "hypothesis_requires_provenance";
    case AutonomyReason::evidence_request_requires_axes: return "evidence_request_requires_axes";
    case AutonomyReason::collect_with_tool_requires_boolean: return "collect_with_tool_requires_boolean";
    case AutonomyReason::evidence_tool_collection_not_configured: return "evidence_tool_collection_not_configured";
    case AutonomyReason::evidence_action_not_allowed: return "evidence_action_not_allowed";
    case AutonomyReason::evidence_action_not_reversible: return "evidence_action_not_reversible";
    case AutonomyReason::evidence_action_confidence_too_low: return "evidence_action_confidence_too_low";
    case AutonomyReason::evidence_action_requires_requested_axes: return "evidence_action_requires_requested_axes";
    case AutonomyReason::evidence_result_requires_boolean_success: return "evidence_result_requires_boolean_success";
    case AutonomyReason::recover_evidence_action: return "recover_evidence_action";
    case AutonomyReason::evidence_failure_budget_exhausted: return "evidence_failure_budget_exhausted";
    case AutonomyReason::verification_requires_accumulator_decision:
        return "verification_requires_accumulator_decision";
    case AutonomyReason::hypothesis_rejected: return "hypothesis_rejected";
    case AutonomyReason::additional_evidence_required: return "additional_evidence_required";
    case AutonomyReason::memory_commit_requires_verified_provenance:
        return "memory_commit_requires_verified_provenance";
    case AutonomyReason::action_not_allowed: return "action_not_allowed";
    case AutonomyReason::action_not_reversible: return "action_not_reversible";
    case AutonomyReason::action_confidence_too_low: return "action_confidence_too_low";
    case AutonomyReason::action_requires_verified_memory: return "action_requires_verified_memory";
    case AutonomyReason::action_result_requires_boolean_success: return "action_result_requires_boolean_success";
    case AutonomyReason::recover_action: return "recover_action";
    case AutonomyReason::failure_budget_exhausted: return "failure_budget_exhausted";
    }
    return "unknown";
}

// SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:89-120
void validate_autonomy_event(const AutonomyEventView& event) {
    if (!detail::is_identity_text(event.event_id)) fail("autonomy_event_invalid:event_id");
    if (!detail::is_identity_text(event.source_family)) fail("autonomy_event_invalid:source_family");
    if (event.hypothesis_id && !detail::is_identity_text(*event.hypothesis_id))
        fail("autonomy_event_invalid:hypothesis_id");
    for (const auto reference : event.evidence_refs)
        if (!detail::is_identity_text(reference)) fail("autonomy_event_invalid:evidence_refs");
    if (!unit_interval(event.confidence)) fail("autonomy_event_invalid:confidence");
    const auto kind = static_cast<std::uint8_t>(event.kind);
    if (kind < 1 || kind > 10) fail("autonomy_event_invalid:kind");
}

// SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:52-86
AutonomyConfig::AutonomyConfig(const AllocationContext& memory, const Input& input)
    : allowed_(memory.allocator<ToolAction>()),
      reversible_(memory.allocator<ToolAction>()),
      evidence_allowed_(memory.allocator<ToolAction>()),
      evidence_reversible_(memory.allocator<ToolAction>()),
      minimum_action_confidence_(input.minimum_action_confidence),
      maximum_action_failures_(input.maximum_action_failures),
      minimum_evidence_action_confidence_(input.minimum_evidence_action_confidence),
      maximum_evidence_action_failures_(input.maximum_evidence_action_failures) {
    allowed_ = action_set(memory, input.allowed_tool_actions);
    reversible_ = action_set(memory, input.reversible_tool_actions);
    if (allowed_.empty()) fail("autonomy_config_invalid:allowed_tool_actions");
    if (!subset(reversible_, allowed_)) fail("autonomy_config_invalid:reversible_tool_actions");
    if (!unit_interval(minimum_action_confidence_)) fail("autonomy_config_invalid:minimum_action_confidence");
    if (maximum_action_failures_ == 0) fail("autonomy_config_invalid:maximum_action_failures");
    evidence_allowed_ = action_set(memory, input.allowed_evidence_tool_actions);
    evidence_reversible_ = action_set(memory, input.reversible_evidence_tool_actions);
    if (!subset(evidence_reversible_, evidence_allowed_))
        fail("autonomy_config_invalid:reversible_evidence_tool_actions");
    if (!unit_interval(minimum_evidence_action_confidence_))
        fail("autonomy_config_invalid:minimum_evidence_action_confidence");
    if (maximum_evidence_action_failures_ == 0) fail("autonomy_config_invalid:maximum_evidence_action_failures");
    Sha256 hash;
    hash_field(hash, "swegca.autonomy_config.v1");
    hash_actions(hash, allowed_);
    hash_actions(hash, reversible_);
    hash_u64(hash, std::bit_cast<std::uint64_t>(minimum_action_confidence_ + 0.0));
    hash_u64(hash, maximum_action_failures_);
    hash_actions(hash, evidence_allowed_);
    hash_actions(hash, evidence_reversible_);
    hash_u64(hash, std::bit_cast<std::uint64_t>(minimum_evidence_action_confidence_ + 0.0));
    hash_u64(hash, maximum_evidence_action_failures_);
    digest_ = Digest256(hash.finish());
}

// SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:41-86
bool AutonomyConfig::allowed(std::string_view action) const noexcept { return contains(allowed_, action); }
// SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:41-86
bool AutonomyConfig::reversible(std::string_view action) const noexcept { return contains(reversible_, action); }
// SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:41-86
bool AutonomyConfig::evidence_allowed(std::string_view action) const noexcept {
    return contains(evidence_allowed_, action);
}
// SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:41-86
bool AutonomyConfig::evidence_reversible(std::string_view action) const noexcept {
    return contains(evidence_reversible_, action);
}

// SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:151-152
AutonomyControl AutonomyControl::initial(const AllocationContext& memory) { return AutonomyControl(memory); }

// version, phase, step, last event, active and verified hypotheses,
// hypothesis confidence, axes, verification status and lower bound, memory
// ref and content hash, pending actions, both failure counts.
// SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:167-186
LedgerBytes AutonomyControl::encode(const AllocationContext& memory) const {
    LedgerBytes out(memory.allocator<std::byte>());
    ByteWriter writer(out);
    const auto phase_value = static_cast<std::uint8_t>(phase);
    const auto status_value = static_cast<std::uint8_t>(verification_status);
    if (phase_value < 1 || phase_value > 10 || status_value > 4 ||
        (hypothesis_confidence && !std::isfinite(*hypothesis_confidence)) ||
        (verification_lower_bound && !std::isfinite(*verification_lower_bound)))
        fail("autonomy_control_invalid");
    writer.u16(control_version);
    writer.u8(phase_value);
    writer.u64(step);
    write_optional(writer, last_event_id);
    write_optional(writer, active_hypothesis_id);
    write_optional(writer, verified_hypothesis_id);
    write_optional(writer, hypothesis_confidence);
    if (requested_axes.size() > std::numeric_limits<std::uint32_t>::max()) fail("autonomy_control_invalid");
    writer.u32(static_cast<std::uint32_t>(requested_axes.size()));
    for (const auto& axis : requested_axes)
        writer.bytes(std::as_bytes(std::span<const char>(axis.data(), axis.size())), payload_limit);
    writer.u8(static_cast<std::uint8_t>(verification_status));
    write_optional(writer, verification_lower_bound);
    write_optional(writer, memory_ref);
    write_optional(writer, memory_content_hash);
    write_optional(writer, pending_tool_action);
    write_optional(writer, pending_evidence_tool_action);
    writer.u64(action_failures);
    writer.u64(evidence_action_failures);
    return out;
}

// SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:167-186
AutonomyControl AutonomyControl::decode(const AllocationContext& memory, std::span<const std::byte> bytes) {
    ByteReader reader(bytes);
    if (reader.u16() != control_version) fail("autonomy_control_invalid");
    AutonomyControl out(memory);
    const auto phase_value = reader.u8();
    if (phase_value < 1 || phase_value > 10) fail("autonomy_control_invalid");
    out.phase = static_cast<AutonomyPhase>(phase_value);
    out.step = reader.u64();
    out.last_event_id = read_optional<AutonomyEventId>(reader, memory);
    out.active_hypothesis_id = read_optional<HypothesisId>(reader, memory);
    out.verified_hypothesis_id = read_optional<HypothesisId>(reader, memory);
    out.hypothesis_confidence = read_optional_double(reader);
    const auto axes = reader.u32();
    if (axes > reader.remaining() / 4) fail("autonomy_control_invalid");
    out.requested_axes.reserve(axes);
    for (std::uint32_t at = 0; at < axes; ++at) {
        auto axis = payload_text(memory, reader.bytes_view(payload_limit));
        if (axis.empty()) fail("autonomy_control_invalid");
        out.requested_axes.push_back(std::move(axis));
    }
    const auto status = reader.u8();
    if (status > 4) fail("autonomy_control_invalid");
    out.verification_status = static_cast<VerificationStatus>(status);
    out.verification_lower_bound = read_optional_double(reader);
    out.memory_ref = read_optional_payload(reader, memory);
    out.memory_content_hash = read_optional_payload(reader, memory);
    out.pending_tool_action = read_optional<ToolAction>(reader, memory);
    out.pending_evidence_tool_action = read_optional<ToolAction>(reader, memory);
    out.action_failures = reader.u64();
    out.evidence_action_failures = reader.u64();
    if (reader.remaining() != 0) fail("autonomy_control_invalid");
    return out;
}

// SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:167-186
Digest256 AutonomyControl::digest(const AllocationContext& memory) const {
    const auto bytes = encode(memory);
    Sha256 hash;
    hash_field(hash, "swegca.autonomy_control.v1");
    hash.update(bytes);
    return Digest256(hash.finish());
}

// SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:189-410
AutonomyStep advance_autonomy(const AutonomyControl& control, const AutonomyEventView& event,
                              const AutonomyConfig& config, const kernel::EvidenceJudgment* decision) noexcept {
    const auto phase = control.phase;
    if (control.last_event_id && control.last_event_id->value() == event.event_id)
        return reject(phase, AutonomyReason::duplicate_event);

    AutonomyStep out;
    out.accepted = true;
    out.reason = AutonomyReason::accepted;
    out.prior_phase = phase;
    auto& update = out.update;

    if (event.kind == AutonomyEventKind::hypothesis_abandon) {
        if (phase != AutonomyPhase::request_evidence && phase != AutonomyPhase::collect_evidence &&
            phase != AutonomyPhase::verify)
            return reject(phase, AutonomyReason::hypothesis_not_abandonable);
        if (!same(event.hypothesis_id, control.active_hypothesis_id))
            return reject(phase, AutonomyReason::hypothesis_mismatch);
        out.next_phase = AutonomyPhase::abstain;
        out.reason = AutonomyReason::hypothesis_abandoned;
        update.verification_status = VerificationStatus::abandoned;
        update.requested_axes = std::span<const std::string_view>();
        return out;
    }
    if (event.kind != expected_event(phase)) return reject(phase, AutonomyReason::unexpected_event_for_phase);
    if (phase != AutonomyPhase::observe && phase != AutonomyPhase::abstain && phase != AutonomyPhase::hypothesize &&
        !same(event.hypothesis_id, control.active_hypothesis_id))
        return reject(phase, AutonomyReason::hypothesis_mismatch);

    using Text = PayloadField<std::string_view>;
    using Flag = PayloadField<bool>;
    const auto& payload = event.payload;
    switch (phase) {
    case AutonomyPhase::observe:
    case AutonomyPhase::abstain:
        out.next_phase = AutonomyPhase::hypothesize;
        update.reset_cycle = true;
        break;
    case AutonomyPhase::hypothesize:
        if (!event.hypothesis_id || event.evidence_refs.empty())
            return reject(phase, AutonomyReason::hypothesis_requires_provenance);
        out.next_phase = AutonomyPhase::request_evidence;
        update.active_hypothesis_id = *event.hypothesis_id;
        update.hypothesis_confidence = event.confidence;
        break;
    case AutonomyPhase::request_evidence: {
        const auto& axes = payload.requested_axes;
        if (axes.state != PayloadField<std::span<const std::string_view>>::State::present || axes.value.empty() ||
            std::any_of(axes.value.begin(), axes.value.end(), [](std::string_view axis) { return axis.empty(); }))
            return reject(phase, AutonomyReason::evidence_request_requires_axes);
        if (payload.collect_with_tool.state == Flag::State::wrong_type)
            return reject(phase, AutonomyReason::collect_with_tool_requires_boolean);
        const bool with_tool =
            payload.collect_with_tool.state == Flag::State::present && payload.collect_with_tool.value;
        if (with_tool && !config.evidence_collection_configured())
            return reject(phase, AutonomyReason::evidence_tool_collection_not_configured);
        out.next_phase = with_tool ? AutonomyPhase::collect_evidence : AutonomyPhase::verify;
        update.requested_axes = axes.value;
        break;
    }
    case AutonomyPhase::collect_evidence: {
        const auto& action = payload.action;
        if (action.state != Text::State::present || !config.evidence_allowed(action.value))
            return reject(phase, AutonomyReason::evidence_action_not_allowed);
        if (!config.evidence_reversible(action.value))
            return reject(phase, AutonomyReason::evidence_action_not_reversible);
        if (event.confidence < config.minimum_evidence_action_confidence())
            return reject(phase, AutonomyReason::evidence_action_confidence_too_low);
        if (control.requested_axes.empty())
            return reject(phase, AutonomyReason::evidence_action_requires_requested_axes);
        out.next_phase = AutonomyPhase::observe_evidence_result;
        out.evidence_tool_action_intent = action.value;
        update.pending_evidence_tool_action.emplace(action.value);
        break;
    }
    case AutonomyPhase::observe_evidence_result: {
        if (payload.success.state != Flag::State::present)
            return reject(phase, AutonomyReason::evidence_result_requires_boolean_success);
        update.pending_evidence_tool_action.emplace(std::nullopt);
        if (payload.success.value) {
            out.next_phase = AutonomyPhase::verify;
            update.evidence_action_failures = 0;
        } else {
            const auto failures = control.evidence_action_failures + 1;
            const bool retry = failures < config.maximum_evidence_action_failures();
            out.next_phase = retry ? AutonomyPhase::collect_evidence : AutonomyPhase::abstain;
            out.reason = retry ? AutonomyReason::recover_evidence_action
                               : AutonomyReason::evidence_failure_budget_exhausted;
            update.evidence_action_failures = failures;
        }
        break;
    }
    case AutonomyPhase::verify:
        if (decision == nullptr) return reject(phase, AutonomyReason::verification_requires_accumulator_decision);
        if (decision->status == kernel::EvidenceStatus::accept) {
            out.next_phase = AutonomyPhase::remember;
            out.memory_write_intent = true;
            update.verified_hypothesis_id.emplace(event.hypothesis_id);
            update.verification_status = VerificationStatus::accept;
            update.verification_lower_bound = decision->causal_lower_bound;
        } else if (decision->status == kernel::EvidenceStatus::reject) {
            out.next_phase = AutonomyPhase::abstain;
            out.reason = AutonomyReason::hypothesis_rejected;
            update.verification_status = VerificationStatus::reject;
        } else {
            out.next_phase = AutonomyPhase::request_evidence;
            out.reason = AutonomyReason::additional_evidence_required;
            update.verification_status = VerificationStatus::abstain;
        }
        break;
    case AutonomyPhase::remember:
        if (!same(event.hypothesis_id, control.verified_hypothesis_id) || !nonempty_text(payload.memory_ref) ||
            !nonempty_text(payload.content_hash) || event.evidence_refs.empty())
            return reject(phase, AutonomyReason::memory_commit_requires_verified_provenance);
        out.next_phase = AutonomyPhase::act;
        update.memory_ref = payload.memory_ref.value;
        update.memory_content_hash = payload.content_hash.value;
        break;
    case AutonomyPhase::act: {
        const auto& action = payload.action;
        if (action.state != Text::State::present || !config.allowed(action.value))
            return reject(phase, AutonomyReason::action_not_allowed);
        if (!config.reversible(action.value)) return reject(phase, AutonomyReason::action_not_reversible);
        if (event.confidence < config.minimum_action_confidence())
            return reject(phase, AutonomyReason::action_confidence_too_low);
        if (!control.memory_ref) return reject(phase, AutonomyReason::action_requires_verified_memory);
        out.next_phase = AutonomyPhase::observe_result;
        out.tool_action_intent = action.value;
        update.pending_tool_action.emplace(action.value);
        break;
    }
    case AutonomyPhase::observe_result: {
        if (payload.success.state != Flag::State::present)
            return reject(phase, AutonomyReason::action_result_requires_boolean_success);
        update.pending_tool_action.emplace(std::nullopt);
        if (payload.success.value) {
            out.next_phase = AutonomyPhase::observe;
            update.action_failures = 0;
        } else {
            const auto failures = control.action_failures + 1;
            const bool retry = failures < config.maximum_action_failures();
            out.next_phase = retry ? AutonomyPhase::act : AutonomyPhase::abstain;
            out.reason = retry ? AutonomyReason::recover_action : AutonomyReason::failure_budget_exhausted;
            update.action_failures = failures;
        }
        break;
    }
    }
    return out;
}

namespace {

// The author's _updated_state: the update, then phase, step and last event.
// SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:167-186
AutonomyControl successor_of(const AllocationContext& memory, const AutonomyControl& control,
                             const AutonomyStep& step, const AutonomyEventView& event) {
    auto next = AutonomyControl::decode(memory, control.encode(memory));  // every field on `memory`
    const auto& update = step.update;
    const auto text = [&memory]<class T>(std::optional<T>& into, const std::optional<std::string_view>& value) {
        if (value) {
            into.emplace(memory, *value);
        } else {
            into.reset();
        }
    };
    if (update.reset_cycle) {
        next.active_hypothesis_id.reset();
        next.verified_hypothesis_id.reset();
        next.memory_ref.reset();
        next.action_failures = 0;
        next.evidence_action_failures = 0;
    }
    if (update.active_hypothesis_id) text(next.active_hypothesis_id, update.active_hypothesis_id);
    if (update.hypothesis_confidence) next.hypothesis_confidence = *update.hypothesis_confidence;
    if (update.requested_axes) {
        next.requested_axes.clear();
        next.requested_axes.reserve(update.requested_axes->size());
        for (const auto axis : *update.requested_axes)
            next.requested_axes.emplace_back(axis.data(), axis.size(), memory.allocator<char>());
    }
    if (update.verification_status) next.verification_status = *update.verification_status;
    if (update.verified_hypothesis_id) text(next.verified_hypothesis_id, *update.verified_hypothesis_id);
    if (update.verification_lower_bound) next.verification_lower_bound = *update.verification_lower_bound;
    if (update.memory_ref) next.memory_ref.emplace(update.memory_ref->data(), update.memory_ref->size(), memory.allocator<char>());
    if (update.memory_content_hash)
        next.memory_content_hash.emplace(update.memory_content_hash->data(), update.memory_content_hash->size(),
                                         memory.allocator<char>());
    if (update.pending_tool_action) text(next.pending_tool_action, *update.pending_tool_action);
    if (update.pending_evidence_tool_action)
        text(next.pending_evidence_tool_action, *update.pending_evidence_tool_action);
    if (update.action_failures) next.action_failures = *update.action_failures;
    if (update.evidence_action_failures) next.evidence_action_failures = *update.evidence_action_failures;
    next.phase = step.next_phase;
    next.step = control.step + 1;
    next.last_event_id.emplace(memory, event.event_id);
    return next;
}

}  // namespace

// SWEGCA: src/swegca/mosaic_autonomous_cognition.py@5901a5a:189-410
AutonomyTransition advance_autonomous_cognition(const AllocationContext& memory, const AutonomyControl& control,
                                                const AutonomyEventView& event, const AutonomyConfig& config,
                                                const EvidenceDecision* decision) {
    validate_autonomy_event(event);
    const auto phase_value = static_cast<std::uint8_t>(control.phase);
    if (phase_value < 1 || phase_value > 10) fail("autonomy_control_invalid");
    const auto step = advance_autonomy(control, event, config, decision ? &decision->judgment() : nullptr);
    AutonomyTransition out(step.accepted ? successor_of(memory, control, step, event)
                                         : AutonomyControl::decode(memory, control.encode(memory)));
    out.accepted_ = step.accepted;
    out.reason_ = step.reason;
    out.prior_phase_ = step.prior_phase;
    out.memory_write_intent_ = step.memory_write_intent;
    if (step.tool_action_intent) out.tool_action_intent_.emplace(memory, *step.tool_action_intent);
    if (step.evidence_tool_action_intent)
        out.evidence_tool_action_intent_.emplace(memory, *step.evidence_tool_action_intent);

    Sha256 hash;
    hash_field(hash, "swegca.autonomy_transition.v1");
    hash.update(control.digest(memory).bytes());
    hash_field(hash, event.event_id);
    hash_u64(hash, static_cast<std::uint8_t>(event.kind));
    hash_optional(hash, event.hypothesis_id);
    hash_u64(hash, event.evidence_refs.size());
    for (const auto reference : event.evidence_refs) hash_field(hash, reference);
    hash_field(hash, event.source_family);
    hash.update(event.context.bytes());
    hash_u64(hash, std::bit_cast<std::uint64_t>(event.confidence + 0.0));
    hash.update(event.payload.digest);
    hash.update(config.digest().bytes());
    hash_u64(hash, decision ? 1 : 0);
    if (decision) hash.update(decision->decision_digest().bytes());
    hash_u64(hash, step.accepted ? 1 : 0);
    hash_u64(hash, static_cast<std::uint8_t>(step.reason));
    hash_u64(hash, static_cast<std::uint8_t>(step.prior_phase));
    hash_u64(hash, static_cast<std::uint8_t>(step.next_phase));
    hash_u64(hash, step.memory_write_intent ? 1 : 0);
    hash_optional(hash, step.tool_action_intent);
    hash_optional(hash, step.evidence_tool_action_intent);
    hash.update(out.successor_.digest(memory).bytes());
    for (const bool flag : {AutonomyTransition::external_action_authorized, AutonomyTransition::memory_write_authorized,
                            AutonomyTransition::tool_action_authorized})
        hash_u64(hash, flag ? 1 : 0);
    out.digest_ = Digest256(hash.finish());
    return out;
}

}  // namespace swegca::architecture
