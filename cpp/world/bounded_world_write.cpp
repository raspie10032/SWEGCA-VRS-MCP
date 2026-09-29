#include "world/bounded_world_write.hpp"

#include "swegca_architecture/sha256.hpp"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <set>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace swegca::world {

class WorldWriteAccess final {
public:
    static const std::shared_ptr<const void>& authority(const WorldWriteGates& gates) {
        return gates.authority_;
    }
    static const std::string& authority_digest(const WorldWriteGates& gates) {
        return gates.authority_digest_;
    }
    static const std::string& proposal_digest(const WorldWriteGates& gates) {
        return gates.proposal_binding_digest_;
    }
};

namespace {

constexpr std::uint64_t verification_index = 30;
constexpr std::string_view write_key = "bounded_verification_write";

const std::shared_ptr<const void>& write_authority() {
    static const auto token = std::static_pointer_cast<const void>(
        std::make_shared<const int>(0));
    return token;
}

std::string hex_digest(const architecture::DigestBytes& digest) {
    constexpr char digits[] = "0123456789abcdef";
    std::string out(digest.size() * 2, '0');
    for (std::size_t index = 0; index != digest.size(); ++index) {
        const auto value = std::to_integer<unsigned>(digest[index]);
        out[index * 2] = digits[value >> 4U];
        out[index * 2 + 1] = digits[value & 15U];
    }
    return out;
}

std::string sha256_text(const std::string_view value) {
    architecture::Sha256 digest;
    digest.update(value);
    return hex_digest(digest.finish());
}

void append_json_string(std::string& out, const std::string_view value) {
    constexpr char digits[] = "0123456789abcdef";
    out.push_back('"');
    std::size_t start = 0;
    for (std::size_t index = 0; index != value.size(); ++index) {
        const auto byte = static_cast<unsigned char>(value[index]);
        if (byte >= 0x20U && byte != '"' && byte != '\\') continue;
        out.append(value.substr(start, index - start));
        if (byte == '"' || byte == '\\') {
            out.push_back('\\');
            out.push_back(static_cast<char>(byte));
        } else {
            switch (byte) {
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                out += "\\u00";
                out.push_back(digits[byte >> 4U]);
                out.push_back(digits[byte & 15U]);
            }
        }
        start = index + 1;
    }
    out.append(value.substr(start));
    out.push_back('"');
}

std::string json_double(const double value) {
    if (std::isnan(value)) return "NaN";
    if (std::isinf(value)) return std::signbit(value) ? "-Infinity" : "Infinity";
    char buffer[64]{};
    const auto [end, error] = std::to_chars(
        buffer, buffer + sizeof(buffer), value, std::chars_format::general);
    if (error != std::errc{}) throw std::runtime_error("JSON number formatting failed");
    std::string out(buffer, end);
    if (out.find_first_of(".eE") == std::string::npos) out += ".0";
    return out;
}

void append_json_value(std::string& out, const JsonValue& value);

void append_json_object(std::string& out, const JsonValue::Object& object) {
    out.push_back('{');
    bool first = true;
    for (const auto& [key, value] : object) {
        if (!first) out.push_back(',');
        first = false;
        append_json_string(out, key);
        out.push_back(':');
        append_json_value(out, value);
    }
    out.push_back('}');
}

void append_json_value(std::string& out, const JsonValue& value) {
    const auto& storage = value.storage();
    if (std::holds_alternative<std::nullptr_t>(storage)) out += "null";
    else if (const auto* item = std::get_if<bool>(&storage)) out += *item ? "true" : "false";
    else if (const auto* item = std::get_if<std::int64_t>(&storage)) out += std::to_string(*item);
    else if (const auto* item = std::get_if<JsonInteger>(&storage)) out += item->value;
    else if (const auto* item = std::get_if<double>(&storage)) out += json_double(*item);
    else if (const auto* item = std::get_if<std::string>(&storage)) append_json_string(out, *item);
    else if (const auto* array = std::get_if<JsonValue::Array>(&storage)) {
        out.push_back('[');
        for (std::size_t index = 0; index != array->size(); ++index) {
            if (index != 0) out.push_back(',');
            append_json_value(out, (*array)[index]);
        }
        out.push_back(']');
    } else append_json_object(out, std::get<JsonValue::Object>(storage));
}

std::string json_strings(const std::span<const std::string> values) {
    std::string out{"["};
    for (std::size_t index = 0; index != values.size(); ++index) {
        if (index != 0) out.push_back(',');
        append_json_string(out, values[index]);
    }
    out.push_back(']');
    return out;
}

std::string dtype_name(const TensorDType dtype) {
    switch (dtype) {
    case TensorDType::bfloat16: return "torch.bfloat16";
    case TensorDType::float16: return "torch.float16";
    case TensorDType::float32: return "torch.float32";
    case TensorDType::float64: return "torch.float64";
    }
    throw std::invalid_argument("unsupported tensor dtype");
}

template <class Integer>
void append_little_endian(std::vector<std::byte>& bytes, const Integer value) {
    using Unsigned = std::make_unsigned_t<Integer>;
    const auto raw = static_cast<Unsigned>(value);
    for (std::size_t index = 0; index != sizeof(Integer); ++index)
        bytes.push_back(static_cast<std::byte>((raw >> (index * 8U)) & 0xffU));
}

std::uint16_t float_to_bfloat16(const float value) {
    auto bits = std::bit_cast<std::uint32_t>(value);
    const std::uint32_t rounding = 0x7fffU + ((bits >> 16U) & 1U);
    bits += rounding;
    return static_cast<std::uint16_t>(bits >> 16U);
}

std::uint16_t float_to_half(const float value) {
    const auto bits = std::bit_cast<std::uint32_t>(value);
    const std::uint32_t sign = (bits >> 16U) & 0x8000U;
    const std::uint32_t exponent = (bits >> 23U) & 0xffU;
    const std::uint32_t fraction = bits & 0x7fffffU;
    if (exponent == 0xffU)
        return static_cast<std::uint16_t>(sign | (fraction == 0 ? 0x7c00U : 0x7e00U));
    const int adjusted = static_cast<int>(exponent) - 127 + 15;
    if (adjusted >= 31) return static_cast<std::uint16_t>(sign | 0x7c00U);
    if (adjusted <= 0) {
        if (adjusted < -10) return static_cast<std::uint16_t>(sign);
        std::uint32_t mantissa = fraction | 0x800000U;
        const unsigned shift = static_cast<unsigned>(14 - adjusted);
        const std::uint32_t halfway = 1U << (shift - 1U);
        const std::uint32_t rounded = (mantissa + halfway - 1U + ((mantissa >> shift) & 1U)) >> shift;
        return static_cast<std::uint16_t>(sign | rounded);
    }
    std::uint32_t mantissa = fraction;
    const std::uint32_t rounded = mantissa + 0xfffU + ((mantissa >> 13U) & 1U);
    std::uint32_t half_exponent = static_cast<std::uint32_t>(adjusted);
    if ((rounded & 0x800000U) != 0) {
        ++half_exponent;
        mantissa = 0;
        if (half_exponent >= 31U) return static_cast<std::uint16_t>(sign | 0x7c00U);
    } else mantissa = rounded;
    return static_cast<std::uint16_t>(sign | (half_exponent << 10U) |
                                      ((mantissa >> 13U) & 0x3ffU));
}

std::string tensor_hash(const Tensor& tensor) {
    architecture::Sha256 digest;
    digest.update(dtype_name(tensor.dtype()));
    std::string shape{"["};
    for (std::size_t index = 0; index != tensor.shape().size(); ++index) {
        if (index != 0) shape.push_back(',');
        shape += std::to_string(tensor.shape()[index]);
    }
    shape.push_back(']');
    digest.update(shape);
    std::vector<std::byte> bytes;
    const std::size_t width = tensor.dtype() == TensorDType::float64 ? 8U :
        tensor.dtype() == TensorDType::float32 ? 4U : 2U;
    bytes.reserve(tensor.values().size() * width);
    for (const double value : tensor.values()) {
        if (tensor.dtype() == TensorDType::float64)
            append_little_endian(bytes, std::bit_cast<std::uint64_t>(value));
        else if (tensor.dtype() == TensorDType::float32)
            append_little_endian(bytes, std::bit_cast<std::uint32_t>(static_cast<float>(value)));
        else if (tensor.dtype() == TensorDType::float16)
            append_little_endian(bytes, float_to_half(static_cast<float>(value)));
        else append_little_endian(bytes, float_to_bfloat16(static_cast<float>(value)));
    }
    digest.update(bytes);
    return hex_digest(digest.finish());
}

std::string mask_hash(const BooleanMask& mask) {
    architecture::Sha256 digest;
    digest.update("torch.bool");
    std::string shape{"["};
    for (std::size_t index = 0; index != mask.shape().size(); ++index) {
        if (index != 0) shape.push_back(',');
        shape += std::to_string(mask.shape()[index]);
    }
    shape.push_back(']');
    digest.update(shape);
    std::vector<std::byte> bytes;
    bytes.reserve(mask.values().size());
    for (const auto value : mask.values()) bytes.push_back(static_cast<std::byte>(value));
    digest.update(bytes);
    return hex_digest(digest.finish());
}

std::string vector_hash(const std::span<const double> values) {
    return tensor_hash(Tensor(TensorDType::float64, {values.size()},
        std::vector<double>(values.begin(), values.end())));
}

std::string proposal_digest(const SynapseProposal& proposal) {
    std::string addresses{"["};
    for (std::size_t batch = 0; batch != proposal.evidence_addresses.size(); ++batch) {
        if (batch != 0) addresses.push_back(',');
        addresses += json_strings(proposal.evidence_addresses[batch]);
    }
    addresses.push_back(']');
    std::string payload{"{\"confidence\":"};
    append_json_string(payload, vector_hash(proposal.confidence));
    payload += ",\"contradiction\":";
    append_json_string(payload, vector_hash(proposal.contradiction));
    payload += ",\"delta_candidate\":";
    append_json_string(payload, tensor_hash(proposal.delta_candidate));
    payload += ",\"evidence_addresses\":" + addresses + ",\"hypothesis_id\":";
    append_json_string(payload, proposal.hypothesis_id);
    payload += ",\"source\":";
    append_json_string(payload, proposal.source);
    payload += ",\"target_slot_mask\":";
    append_json_string(payload, mask_hash(proposal.target_slot_mask));
    payload += ",\"uncertainty\":";
    append_json_string(payload, vector_hash(proposal.uncertainty));
    payload.push_back('}');
    return sha256_text(payload);
}

std::string gate_digest(const WorldWriteGates& gates,
                        const std::string_view bound_proposal) {
    std::string payload{"{\"accumulator_revision_current\":"};
    payload += gates.accumulator_revision_current ? "true" : "false";
    payload += ",\"capacity_strategy_safe\":";
    payload += gates.capacity_strategy_safe ? "true" : "false";
    payload += ",\"causal_lower_bound\":" + json_double(gates.causal_lower_bound);
    payload += ",\"context_diversity\":" + std::to_string(gates.context_diversity);
    payload += ",\"counterfactual_support\":";
    payload += gates.counterfactual_support ? "true" : "false";
    payload += ",\"definitions_complete\":";
    payload += gates.definitions_complete ? "true" : "false";
    payload += ",\"device_gate_passed\":";
    payload += gates.device_gate_passed ? "true" : "false";
    payload += ",\"evidence_current\":";
    payload += gates.evidence_current ? "true" : "false";
    payload += ",\"evidence_status\":";
    append_json_string(payload, gates.evidence_status);
    payload += ",\"intervention_support\":";
    payload += gates.intervention_support ? "true" : "false";
    payload += ",\"proposal_binding_digest\":";
    append_json_string(payload, bound_proposal);
    payload += ",\"regime_change_suspected\":";
    payload += gates.regime_change_suspected ? "true" : "false";
    payload += ",\"runtime_context_safe\":";
    payload += gates.runtime_context_safe ? "true" : "false";
    payload += ",\"slot_gate_passed\":";
    payload += gates.slot_gate_passed ? "true" : "false";
    payload += ",\"source_diversity\":" + std::to_string(gates.source_diversity) + "}";
    return sha256_text(payload);
}

const RevisionPayloadValue* payload_entry(const EvidenceRevisionVerification& verification,
                                          const std::string_view key) {
    const RevisionPayloadValue* found = nullptr;
    for (const auto& [candidate, value] : verification.decision_payload)
        if (candidate == key) found = &value;
    return found;
}

bool payload_string(const RevisionPayloadValue* value, const std::string& expected) {
    const auto* text = value == nullptr ? nullptr : std::get_if<std::string>(value);
    return text != nullptr && *text == expected;
}

bool payload_double(const RevisionPayloadValue* value, const double expected) {
    if (value == nullptr) return false;
    if (const auto* number = std::get_if<double>(value)) return *number == expected;
    if (const auto* integer = std::get_if<std::int64_t>(value)) {
        constexpr double two_to_63 = 9223372036854775808.0;
        if (!std::isfinite(expected) || std::trunc(expected) != expected ||
            expected < -two_to_63 || expected >= two_to_63)
            return false;
        return static_cast<std::int64_t>(expected) == *integer;
    }
    if (const auto* boolean = std::get_if<bool>(value))
        return expected == (*boolean ? 1.0 : 0.0);
    return false;
}

bool payload_unsigned_integer(const RevisionPayloadValue* value,
                              const std::uint64_t expected) {
    constexpr double two_to_64 = 18446744073709551616.0;
    if (value == nullptr) return false;
    if (const auto* integer = std::get_if<std::int64_t>(value))
        return *integer >= 0 && static_cast<std::uint64_t>(*integer) == expected;
    if (const auto* number = std::get_if<double>(value)) {
        if (!std::isfinite(*number) || std::trunc(*number) != *number ||
            *number < 0.0 || *number >= two_to_64)
            return false;
        return static_cast<std::uint64_t>(*number) == expected;
    }
    if (const auto* boolean = std::get_if<bool>(value))
        return static_cast<std::uint64_t>(*boolean) == expected;
    return false;
}

bool payload_size(const RevisionPayloadValue* value, const std::size_t expected) {
    static_assert(sizeof(std::size_t) <= sizeof(std::uint64_t));
    return payload_unsigned_integer(value, static_cast<std::uint64_t>(expected));
}

bool verification_matches_decision(const EvidenceRevisionVerification& verification,
                                   const AccumulatorDecision& decision) {
    const auto* addresses = payload_entry(verification, "evidence_addresses");
    const auto* stored_addresses = addresses == nullptr ? nullptr :
        std::get_if<std::vector<std::string>>(addresses);
    return payload_string(payload_entry(verification, "status"), decision.status) &&
        payload_string(payload_entry(verification, "reason"), decision.reason) &&
        payload_double(payload_entry(verification, "posterior_mean"), decision.posterior_mean) &&
        payload_double(payload_entry(verification, "causal_lower_bound"), decision.causal_lower_bound) &&
        payload_double(payload_entry(verification, "overall_upper_bound"), decision.overall_upper_bound) &&
        payload_double(payload_entry(verification, "effective_sample_size"), decision.effective_sample_size) &&
        payload_size(payload_entry(verification, "source_diversity"), decision.source_diversity) &&
        payload_size(payload_entry(verification, "context_diversity"), decision.context_diversity) &&
        payload_double(payload_entry(verification, "regime_change_score"), decision.regime_change_score) &&
        payload_unsigned_integer(payload_entry(verification, "revision"), decision.revision) &&
        payload_string(payload_entry(verification, "hypothesis_id"), decision.hypothesis_id) &&
        stored_addresses != nullptr && *stored_addresses == decision.evidence_addresses;
}

std::vector<std::string> flattened_addresses(const SynapseProposal& proposal) {
    std::vector<std::string> result;
    for (const auto& batch : proposal.evidence_addresses)
        result.insert(result.end(), batch.begin(), batch.end());
    return result;
}

struct VerificationTopology final {
    std::uint64_t semantic_slots;
    std::uint64_t executive_slots;
    std::uint64_t scratch_slots;
    std::uint64_t local_index;
};

VerificationTopology verification_topology(const CognitiveState& state) {
    const Tensor* partitions[]{&state.semantic_slots(), &state.executive_slots(),
                               &state.scratch_slots()};
    for (const auto* tensor : partitions)
        if (tensor->rank() != 3)
            throw std::invalid_argument("cognitive slot partitions must have rank three");
    const auto semantic = state.semantic_slots().shape();
    const auto executive = state.executive_slots().shape();
    const auto scratch = state.scratch_slots().shape();
    if (semantic[0] != executive[0] || semantic[0] != scratch[0] ||
        semantic[2] != executive[2] || semantic[2] != scratch[2])
        throw std::invalid_argument("cognitive slot partitions must share batch and dimension");
    if (semantic[1] > slot_roles.size() || executive[1] > slot_roles.size() ||
        scratch[1] > slot_roles.size())
        throw std::invalid_argument(
            "compatibility slot topology requires exactly the registered 32 roles");
    const auto prefix = semantic[1] + executive[1];
    const auto total = prefix + scratch[1];
    if (total != slot_roles.size())
        throw std::invalid_argument(
            "compatibility slot topology requires exactly the registered 32 roles");
    if (verification_index < prefix || verification_index >= total ||
        verification_index - prefix >= scratch[1])
        throw std::invalid_argument("verification role must remain in the scratch partition");
    return VerificationTopology{
        semantic[1], executive[1], scratch[1], verification_index - prefix};
}

std::optional<std::string> authorization_reason(const WorldWriteGates& gates,
                                                const BoundedWorldWriteConfig& config) {
    if (gates.evidence_status != "accept") return "evidence_not_accepted";
    if (!gates.evidence_current) return "evidence_expired";
    if (!gates.accumulator_revision_current) return "accumulator_revision_stale";
    if (!gates.runtime_context_safe) return "runtime_context_unsafe";
    if (gates.causal_lower_bound < config.minimum_causal_lower_bound)
        return "causal_lower_bound";
    if (gates.source_diversity < config.minimum_source_diversity)
        return "source_diversity";
    if (gates.context_diversity < config.minimum_context_diversity)
        return "context_diversity";
    if (!gates.definitions_complete) return "definitions_incomplete";
    if (!gates.counterfactual_support) return "counterfactual_support";
    if (!gates.intervention_support) return "intervention_support";
    if (gates.regime_change_suspected) return "regime_change_suspected";
    if (!gates.slot_gate_passed) return "slot_gate";
    if (!gates.device_gate_passed) return "device_gate";
    if (!gates.capacity_strategy_safe) return "capacity_strategy";
    return std::nullopt;
}

Tensor slot_tensor(const CognitiveState& state, const std::uint64_t local_slot) {
    const auto shape = state.scratch_slots().shape();
    std::vector<double> values;
    values.reserve(static_cast<std::size_t>(shape[0] * shape[2]));
    for (std::uint64_t batch = 0; batch != shape[0]; ++batch) {
        const auto begin = static_cast<std::size_t>((batch * shape[1] + local_slot) * shape[2]);
        values.insert(values.end(),
            state.scratch_slots().values().begin() + static_cast<std::ptrdiff_t>(begin),
            state.scratch_slots().values().begin() +
                static_cast<std::ptrdiff_t>(begin + shape[2]));
    }
    return Tensor(state.scratch_slots().dtype(), {shape[0], shape[2]},
                  std::move(values), std::string(state.scratch_slots().device()));
}

Tensor delta_slot(const Tensor& delta, const std::uint64_t slot) {
    const auto shape = delta.shape();
    std::vector<double> values;
    values.reserve(static_cast<std::size_t>(shape[0] * shape[2]));
    for (std::uint64_t batch = 0; batch != shape[0]; ++batch) {
        const auto begin = static_cast<std::size_t>((batch * shape[1] + slot) * shape[2]);
        values.insert(values.end(), delta.values().begin() + static_cast<std::ptrdiff_t>(begin),
            delta.values().begin() + static_cast<std::ptrdiff_t>(begin + shape[2]));
    }
    return Tensor(delta.dtype(), {shape[0], shape[2]}, std::move(values),
                  std::string(delta.device()));
}

Tensor add_tensors(const Tensor& left, const Tensor& right) {
    std::vector<double> values(left.values().begin(), left.values().end());
    for (std::size_t index = 0; index != values.size(); ++index)
        values[index] += right.values()[index];
    return Tensor(left.dtype(), std::vector<std::uint64_t>(left.shape().begin(), left.shape().end()),
                  std::move(values), std::string(left.device()));
}

Tensor replace_scratch_slot(const Tensor& scratch, const std::uint64_t local_slot,
                            const Tensor& value) {
    const auto shape = scratch.shape();
    const auto value_shape = value.shape();
    if (value_shape.size() != 2 || value_shape[0] != shape[0] ||
        value_shape[1] != shape[2])
        throw std::invalid_argument("verification slot value has incompatible shape");
    std::vector<double> values(scratch.values().begin(), scratch.values().end());
    for (std::uint64_t batch = 0; batch != shape[0]; ++batch) {
        const auto target = static_cast<std::size_t>((batch * shape[1] + local_slot) * shape[2]);
        const auto source = static_cast<std::size_t>(batch * shape[2]);
        std::copy_n(value.values().begin() + static_cast<std::ptrdiff_t>(source),
                    static_cast<std::size_t>(shape[2]),
                    values.begin() + static_cast<std::ptrdiff_t>(target));
    }
    return Tensor(scratch.dtype(), std::vector<std::uint64_t>(shape.begin(), shape.end()),
                  std::move(values), std::string(scratch.device()));
}

std::shared_ptr<const CognitiveState> replace_scratch_and_self(
    const CognitiveState& state, Tensor scratch, JsonValue::Object self_state) {
    return std::make_shared<const CognitiveState>(
        state.semantic_slots().clone(), state.executive_slots().clone(),
        std::move(scratch), state.structured_world_graph(),
        std::vector<std::string>(state.evidence_refs().begin(), state.evidence_refs().end()),
        state.goal_state(), state.value_state(), std::move(self_state),
        std::string(state.owner_id()));
}

const JsonValue* optional_value(const JsonValue::Object& object,
                                const std::string_view key) {
    const auto found = object.find(key);
    return found == object.end() ? nullptr : &found->second;
}

std::string python_string(const JsonValue& value) {
    const auto& storage = value.storage();
    if (const auto* item = std::get_if<std::string>(&storage)) return *item;
    if (const auto* item = std::get_if<std::int64_t>(&storage)) return std::to_string(*item);
    if (const auto* item = std::get_if<double>(&storage)) {
        if (std::isnan(*item)) return "nan";
        if (std::isinf(*item)) return std::signbit(*item) ? "-inf" : "inf";
        return json_double(*item);
    }
    if (const auto* item = std::get_if<bool>(&storage)) return *item ? "True" : "False";
    if (std::holds_alternative<std::nullptr_t>(storage)) return "None";
    throw std::invalid_argument("receipt identification field must be scalar");
}

std::string required_string(const JsonValue::Object& object,
                            const std::string_view key) {
    const auto* value = optional_value(object, key);
    if (value == nullptr) throw std::out_of_range("missing receipt field: " + std::string(key));
    return python_string(*value);
}

std::vector<std::string> receipt_strings(const JsonValue* value) {
    std::vector<std::string> result;
    if (value == nullptr) return result;
    result.reserve(value->as_array().size());
    for (const auto& item : value->as_array()) result.emplace_back(item.as_string());
    return result;
}

std::int64_t python_int(const JsonValue& value) {
    constexpr double two_to_63 = 9223372036854775808.0;
    const auto& storage = value.storage();
    if (const auto* item = std::get_if<std::int64_t>(&storage)) return *item;
    if (const auto* item = std::get_if<bool>(&storage)) return *item ? 1 : 0;
    if (const auto* item = std::get_if<double>(&storage)) {
        if (!std::isfinite(*item) || *item < -two_to_63 || *item >= two_to_63)
            throw std::invalid_argument("bounded write revision is invalid");
        return static_cast<std::int64_t>(*item);
    }
    if (const auto* item = std::get_if<std::string>(&storage)) {
        const auto first = item->find_first_not_of(" \t\n\r\f\v");
        if (first == std::string::npos)
            throw std::invalid_argument("bounded write revision is invalid");
        const auto last = item->find_last_not_of(" \t\n\r\f\v");
        std::string_view text(*item);
        text = text.substr(first, last - first + 1);
        if (!text.empty() && text.front() == '+') {
            text.remove_prefix(1);
        }
        if (text.empty()) throw std::invalid_argument("bounded write revision is invalid");
        std::int64_t result = 0;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
        if (parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size()) {
            return result;
        }
    }
    throw std::invalid_argument("bounded write revision is invalid");
}

bool exact_json_integer_equal(const JsonValue& value, const std::int64_t expected) {
    constexpr double two_to_63 = 9223372036854775808.0;
    const auto& storage = value.storage();
    if (const auto* item = std::get_if<std::int64_t>(&storage)) return *item == expected;
    if (const auto* item = std::get_if<bool>(&storage)) return (*item ? 1 : 0) == expected;
    if (const auto* item = std::get_if<double>(&storage)) {
        if (!std::isfinite(*item) || std::trunc(*item) != *item ||
            *item < -two_to_63 || *item >= two_to_63)
            return false;
        return static_cast<std::int64_t>(*item) == expected;
    }
    return false;
}

std::string receipt_id(const std::string& before_state_hash,
                       const std::string& delta_hash, const std::int64_t revision,
                       const std::vector<std::string>& evidence_refs,
                       const std::string& hypothesis_id,
                       const std::string& bound_proposal) {
    std::string payload{"{\"before_state_hash\":"};
    append_json_string(payload, before_state_hash);
    payload += ",\"delta_hash\":";
    append_json_string(payload, delta_hash);
    payload += ",\"evidence_refs\":" + json_strings(evidence_refs);
    payload += ",\"hypothesis_id\":";
    append_json_string(payload, hypothesis_id);
    payload += ",\"proposal_binding_digest\":";
    append_json_string(payload, bound_proposal);
    payload += ",\"revision\":" + std::to_string(revision) + "}";
    return sha256_text(payload);
}

}  // namespace

// Kept out of the public header: this narrow probe lets the direct regression
// tests exercise the exact payload comparison without minting authority or
// manufacturing 2^53 contiguous evidence-ledger rows.
bool bounded_world_write_payload_matches_for_test(
    const EvidenceRevisionVerification& verification,
    const AccumulatorDecision& decision) {
    return verification_matches_decision(verification, decision);
}

BoundedWorldWriteConfig::BoundedWorldWriteConfig(
    const double minimum_causal_lower_bound_value,
    const std::size_t minimum_source_diversity_value,
    const std::size_t minimum_context_diversity_value,
    const double maximum_slot_delta_value,
    const double minimum_proposal_weight_value)
    : minimum_causal_lower_bound(minimum_causal_lower_bound_value),
      minimum_source_diversity(minimum_source_diversity_value),
      minimum_context_diversity(minimum_context_diversity_value),
      maximum_slot_delta(maximum_slot_delta_value),
      minimum_proposal_weight(minimum_proposal_weight_value) {
    for (const double value : {minimum_causal_lower_bound, maximum_slot_delta,
                               minimum_proposal_weight})
        if (!std::isfinite(value) || value < 0.0 || value > 1.0)
            throw std::invalid_argument("bounded write probability must be finite within [0, 1]");
    if (maximum_slot_delta <= 0.0)
        throw std::invalid_argument("maximum slot delta must be positive");
    if (std::min(minimum_source_diversity, minimum_context_diversity) == 0)
        throw std::invalid_argument("diversity minima must be positive");
}

WorldWriteGates::WorldWriteGates(
    std::string evidence_status_value, const double causal_lower_bound_value,
    const std::size_t source_diversity_value, const std::size_t context_diversity_value,
    const bool definitions_complete_value, const bool counterfactual_support_value,
    const bool intervention_support_value, const bool regime_change_suspected_value,
    const bool slot_gate_passed_value, const bool device_gate_passed_value,
    const bool capacity_strategy_safe_value, const bool evidence_current_value,
    const bool accumulator_revision_current_value, const bool runtime_context_safe_value)
    : WorldWriteGates(std::move(evidence_status_value), causal_lower_bound_value,
          source_diversity_value, context_diversity_value, definitions_complete_value,
          counterfactual_support_value, intervention_support_value,
          regime_change_suspected_value, slot_gate_passed_value,
          device_gate_passed_value, capacity_strategy_safe_value,
          evidence_current_value, accumulator_revision_current_value,
          runtime_context_safe_value, {}, {}, {}) {}

WorldWriteGates::WorldWriteGates(
    std::string evidence_status_value, const double causal_lower_bound_value,
    const std::size_t source_diversity_value, const std::size_t context_diversity_value,
    const bool definitions_complete_value, const bool counterfactual_support_value,
    const bool intervention_support_value, const bool regime_change_suspected_value,
    const bool slot_gate_passed_value, const bool device_gate_passed_value,
    const bool capacity_strategy_safe_value, const bool evidence_current_value,
    const bool accumulator_revision_current_value, const bool runtime_context_safe_value,
    std::shared_ptr<const void> authority, std::string authority_digest,
    std::string proposal_binding_digest_value)
    : evidence_status(std::move(evidence_status_value)),
      causal_lower_bound(causal_lower_bound_value),
      source_diversity(source_diversity_value), context_diversity(context_diversity_value),
      definitions_complete(definitions_complete_value),
      counterfactual_support(counterfactual_support_value),
      intervention_support(intervention_support_value),
      regime_change_suspected(regime_change_suspected_value),
      slot_gate_passed(slot_gate_passed_value), device_gate_passed(device_gate_passed_value),
      capacity_strategy_safe(capacity_strategy_safe_value),
      evidence_current(evidence_current_value),
      accumulator_revision_current(accumulator_revision_current_value),
      runtime_context_safe(runtime_context_safe_value), authority_(std::move(authority)),
      authority_digest_(std::move(authority_digest)),
      proposal_binding_digest_(std::move(proposal_binding_digest_value)) {
    if (!std::isfinite(causal_lower_bound) || causal_lower_bound < 0.0 ||
        causal_lower_bound > 1.0)
        throw std::invalid_argument("causal lower bound must be finite within [0, 1]");
}

WorldWriteGates world_write_gates_from_decision(
    const AccumulatorDecision& decision, const SynapseProposal& proposal,
    const bool definitions_complete, const bool counterfactual_support,
    const bool intervention_support, const bool regime_change_suspected,
    const bool slot_gate_passed, const bool device_gate_passed,
    const bool capacity_strategy_safe, const bool evidence_current,
    const bool accumulator_revision_current, const bool runtime_context_safe,
    const EvidenceRevisionVerification* revision_verification) {
    bool authoritative = is_authoritative_accumulator_decision(decision);
    if (!authoritative && revision_verification != nullptr)
        authoritative = is_authoritative_evidence_revision(*revision_verification) &&
            revision_verification->evidence_current &&
            revision_verification->accumulator_revision_current &&
            verification_matches_decision(*revision_verification, decision);
    if (!authoritative)
        throw AuthorityError("World write evidence is not an authority capability");
    const std::set<std::string> accepted(decision.evidence_addresses.begin(),
                                         decision.evidence_addresses.end());
    const auto proposed = flattened_addresses(proposal);
    if (decision.hypothesis_id.empty() || accepted.empty())
        throw AuthorityError("World write decision has no bound evidence history");
    if (proposal.hypothesis_id != decision.hypothesis_id)
        throw AuthorityError("World write proposal hypothesis is not accepted");
    if (proposed.empty() || std::any_of(proposed.begin(), proposed.end(),
                                        [](const auto& value) { return value.empty(); }))
        throw AuthorityError("World write proposal has no accepted evidence");
    if (std::any_of(proposed.begin(), proposed.end(),
                    [&](const auto& value) { return !accepted.contains(value); }))
        throw AuthorityError("World write proposal cites unaccepted evidence");
    const std::string bound = proposal_digest(proposal);
    WorldWriteGates plain(decision.status, decision.causal_lower_bound,
        decision.source_diversity, decision.context_diversity,
        definitions_complete, counterfactual_support, intervention_support,
        regime_change_suspected, slot_gate_passed, device_gate_passed,
        capacity_strategy_safe, evidence_current, accumulator_revision_current,
        runtime_context_safe);
    const std::string digest = gate_digest(plain, bound);
    return WorldWriteGates(decision.status, decision.causal_lower_bound,
        decision.source_diversity, decision.context_diversity,
        definitions_complete, counterfactual_support, intervention_support,
        regime_change_suspected, slot_gate_passed, device_gate_passed,
        capacity_strategy_safe, evidence_current, accumulator_revision_current,
        runtime_context_safe, write_authority(), digest, bound);
}

BoundedWorldWriteReceipt::BoundedWorldWriteReceipt(
    std::string receipt_id_value, const std::int64_t revision_value,
    std::string target_role_value, std::string before_state_hash_value,
    std::string after_state_hash_value, Tensor before_slot_value,
    std::string before_slot_hash_value, std::string after_slot_hash_value,
    std::string applied_delta_hash_value, std::vector<std::string> evidence_refs_value,
    std::optional<JsonValue::Object> prior_write_metadata_value,
    std::string hypothesis_id_value, std::string proposal_binding_digest_value)
    : receipt_id(std::move(receipt_id_value)), revision(revision_value),
      target_role(std::move(target_role_value)),
      before_state_hash(std::move(before_state_hash_value)),
      after_state_hash(std::move(after_state_hash_value)),
      before_slot(std::move(before_slot_value)),
      before_slot_hash(std::move(before_slot_hash_value)),
      after_slot_hash(std::move(after_slot_hash_value)),
      applied_delta_hash(std::move(applied_delta_hash_value)),
      evidence_refs(std::move(evidence_refs_value)),
      prior_write_metadata(std::move(prior_write_metadata_value)),
      hypothesis_id(std::move(hypothesis_id_value)),
      proposal_binding_digest(std::move(proposal_binding_digest_value)) {}

BoundedWorldWriteResult::BoundedWorldWriteResult(
    std::shared_ptr<const CognitiveState> state_value, const bool authorized_value,
    const bool committed_value, std::string reason_value, Tensor proposed_delta_value,
    std::shared_ptr<const BoundedWorldWriteReceipt> receipt_value)
    : state(std::move(state_value)), authorized(authorized_value),
      committed(committed_value), reason(std::move(reason_value)),
      proposed_delta(std::move(proposed_delta_value)), receipt(std::move(receipt_value)) {}

JsonValue::Object bounded_world_write_receipt_to_dict(
    const BoundedWorldWriteReceipt& receipt) {
    JsonValue::Array evidence;
    evidence.reserve(receipt.evidence_refs.size());
    for (const auto& item : receipt.evidence_refs) evidence.emplace_back(item);
    return JsonValue::Object{
        {"receipt_id", receipt.receipt_id},
        {"revision", receipt.revision},
        {"target_role", receipt.target_role},
        {"before_state_hash", receipt.before_state_hash},
        {"after_state_hash", receipt.after_state_hash},
        {"before_slot", receipt.before_slot.to_json_payload()},
        {"before_slot_hash", receipt.before_slot_hash},
        {"after_slot_hash", receipt.after_slot_hash},
        {"applied_delta_hash", receipt.applied_delta_hash},
        {"evidence_refs", std::move(evidence)},
        {"hypothesis_id", receipt.hypothesis_id},
        {"proposal_binding_digest", receipt.proposal_binding_digest},
        {"prior_write_metadata", receipt.prior_write_metadata
            ? JsonValue(*receipt.prior_write_metadata) : JsonValue(nullptr)},
    };
}

BoundedWorldWriteReceipt bounded_world_write_receipt_from_dict(
    const JsonValue::Object& payload) {
    const auto* before_slot_payload = optional_value(payload, "before_slot");
    if (before_slot_payload == nullptr || !before_slot_payload->is_object())
        throw std::invalid_argument("receipt before slot must be an object");
    const auto* revision_value = optional_value(payload, "revision");
    if (revision_value == nullptr) throw std::out_of_range("missing receipt field: revision");
    const std::int64_t revision = python_int(*revision_value);

    std::optional<JsonValue::Object> prior;
    if (const auto* value = optional_value(payload, "prior_write_metadata");
        value != nullptr && !std::holds_alternative<std::nullptr_t>(value->storage())) {
        if (!value->is_object())
            throw std::invalid_argument("prior write metadata must be an object or null");
        prior = value->as_object();
    }
    const auto* hypothesis = optional_value(payload, "hypothesis_id");
    const auto* binding = optional_value(payload, "proposal_binding_digest");
    return BoundedWorldWriteReceipt(
        required_string(payload, "receipt_id"), revision,
        required_string(payload, "target_role"),
        required_string(payload, "before_state_hash"),
        required_string(payload, "after_state_hash"),
        Tensor::from_json_payload(*before_slot_payload),
        required_string(payload, "before_slot_hash"),
        required_string(payload, "after_slot_hash"),
        required_string(payload, "applied_delta_hash"),
        receipt_strings(optional_value(payload, "evidence_refs")), std::move(prior),
        hypothesis == nullptr ? std::string{} : python_string(*hypothesis),
        binding == nullptr ? std::string{} : python_string(*binding));
}

std::string cognitive_state_hash(const CognitiveState& state) {
    architecture::Sha256 digest;
    for (const Tensor* tensor : {&state.semantic_slots(), &state.executive_slots(),
                                 &state.scratch_slots()})
        digest.update(tensor_hash(*tensor));
    JsonValue::Array evidence;
    for (const auto& reference : state.evidence_refs()) evidence.emplace_back(reference);
    JsonValue::Object metadata{
        {"structured_world_graph", state.structured_world_graph().to_json()},
        {"evidence_refs", std::move(evidence)},
        {"goal_state", state.goal_state()},
        {"value_state", state.value_state()},
        {"self_state", state.self_state()},
        {"owner_id", std::string(state.owner_id())},
    };
    std::string encoded;
    append_json_object(encoded, metadata);
    digest.update(encoded);
    return hex_digest(digest.finish());
}

BoundedWorldWriteResult bounded_verification_write(
    std::shared_ptr<const CognitiveState> state,
    const SynapseProposal& proposal, const WorldWriteGates& gates,
    const BoundedWorldWriteConfig& config, const bool commit) {
    if (!state) throw std::invalid_argument("bounded World write state must not be null");
    const auto topology = verification_topology(*state);
    const auto semantic = state->semantic_slots().shape();
    if (state->persistent_state_count() != 1 || semantic[0] != 1)
        throw std::invalid_argument("bounded World write requires one batch-one CognitiveState");
    proposal.validate(*state);
    std::size_t targets = 0;
    std::uint64_t target_batch = 0;
    std::uint64_t target_slot = 0;
    for (std::uint64_t batch = 0; batch != proposal.target_slot_mask.shape()[0]; ++batch)
        for (std::uint64_t slot = 0; slot != proposal.target_slot_mask.shape()[1]; ++slot)
            if (proposal.target_slot_mask.at(batch, slot)) {
                ++targets; target_batch = batch; target_slot = slot;
            }
    if (targets != 1 || target_batch != 0 || target_slot != verification_index)
        throw std::invalid_argument("bounded write proposal must target only verification");
    auto reason = authorization_reason(gates, config);
    SingleWorldArbiter arbiter(config.maximum_slot_delta, config.maximum_slot_delta,
                               config.minimum_proposal_weight);
    const std::array<SynapseProposal, 1> proposals{proposal};
    auto arbitration = arbiter(state, proposals, false);
    if (!reason && std::any_of(arbitration.accepted().values().begin(),
                               arbitration.accepted().values().end(),
                               [](const auto value) { return value == 0; }))
        reason = "proposal_weight";
    if (reason)
        return BoundedWorldWriteResult(state, false, false, *reason,
                                       arbitration.proposed_delta().clone());
    const auto& bound = WorldWriteAccess::proposal_digest(gates);
    if (WorldWriteAccess::authority(gates) != write_authority() ||
        WorldWriteAccess::authority_digest(gates) != gate_digest(gates, bound))
        throw AuthorityError("World write gates are not an authority capability");
    if (bound != proposal_digest(proposal))
        throw AuthorityError("World write proposal does not match authority binding");
    if (!commit)
        return BoundedWorldWriteResult(state, true, false, "authorized_dry_run",
                                       arbitration.proposed_delta().clone());

    const auto local_index = topology.local_index;
    const std::string before_hash = cognitive_state_hash(*state);
    const Tensor before_slot = slot_tensor(*state, local_index);
    const Tensor local_delta = delta_slot(arbitration.proposed_delta(), verification_index);
    const Tensor after_slot = add_tensors(before_slot, local_delta);
    const Tensor scratch_after = replace_scratch_slot(state->scratch_slots(), local_index, after_slot);
    const auto prior_it = state->self_state().find(std::string(write_key));
    std::optional<JsonValue::Object> prior;
    if (prior_it != state->self_state().end()) {
        if (!prior_it->second.is_object())
            throw std::invalid_argument("bounded write metadata must be an object");
        if (!prior_it->second.as_object().empty()) prior = prior_it->second.as_object();
    }
    std::int64_t revision = 1;
    if (prior) {
        const auto found = prior->find("revision");
        const std::int64_t previous = found == prior->end() ? 0 : python_int(found->second);
        if (previous == std::numeric_limits<std::int64_t>::max())
            throw std::overflow_error("bounded write revision overflow");
        revision = previous + 1;
    }
    const auto evidence_refs = flattened_addresses(proposal);
    const std::string delta_hash = tensor_hash(local_delta);
    const std::string id = receipt_id(before_hash, delta_hash, revision, evidence_refs,
                                      proposal.hypothesis_id, bound);
    JsonValue::Array evidence_json;
    for (const auto& value : evidence_refs) evidence_json.emplace_back(value);
    JsonValue::Object write_metadata{
        {"policy_version", "bounded-verification-v1"},
        {"receipt_id", id},
        {"revision", revision},
        {"target_role", "verification"},
        {"evidence_refs", std::move(evidence_json)},
        {"hypothesis_id", proposal.hypothesis_id},
        {"proposal_binding_digest", bound},
    };
    auto self_state = state->self_state();
    self_state[std::string(write_key)] = JsonValue(write_metadata);
    const auto updated = std::make_shared<const CognitiveState>(
        state->semantic_slots().clone(), state->executive_slots().clone(), scratch_after,
        state->structured_world_graph(),
        std::vector<std::string>(state->evidence_refs().begin(), state->evidence_refs().end()),
        state->goal_state(), state->value_state(), std::move(self_state),
        std::string(state->owner_id()));
    const std::string after_hash = cognitive_state_hash(*updated);
    const auto receipt = std::make_shared<const BoundedWorldWriteReceipt>(
        id, revision, "verification", before_hash, after_hash, before_slot.clone(),
        tensor_hash(before_slot), tensor_hash(after_slot), delta_hash, evidence_refs,
        prior, proposal.hypothesis_id, bound);
    return BoundedWorldWriteResult(updated, true, true, "committed",
                                   arbitration.proposed_delta().clone(), receipt);
}

std::shared_ptr<const CognitiveState> rollback_bounded_verification_write(
    std::shared_ptr<const CognitiveState> state,
    const BoundedWorldWriteReceipt& receipt) {
    if (!state) throw std::invalid_argument("bounded rollback state must not be null");
    if (receipt.target_role != "verification")
        throw std::invalid_argument("receipt target role is invalid");
    const auto topology = verification_topology(*state);
    if (cognitive_state_hash(*state) != receipt.after_state_hash)
        throw std::invalid_argument("state changed after bounded write; rollback is stale");
    const auto local_index = topology.local_index;
    if (tensor_hash(slot_tensor(*state, local_index)) != receipt.after_slot_hash)
        throw std::invalid_argument("verification slot differs from receipt");
    const auto before_slot = Tensor(
        state->scratch_slots().dtype(),
        std::vector<std::uint64_t>(receipt.before_slot.shape().begin(),
                                   receipt.before_slot.shape().end()),
        std::vector<double>(receipt.before_slot.values().begin(),
                            receipt.before_slot.values().end()),
        std::string(state->scratch_slots().device()));
    auto self_state = state->self_state();
    if (receipt.prior_write_metadata) {
        self_state[std::string(write_key)] = JsonValue(*receipt.prior_write_metadata);
    } else {
        self_state.erase(std::string(write_key));
    }
    auto restored = replace_scratch_and_self(
        *state, replace_scratch_slot(state->scratch_slots(), local_index, before_slot),
        std::move(self_state));
    if (cognitive_state_hash(*restored) != receipt.before_state_hash)
        throw std::invalid_argument("bounded write rollback was not bit-exact");
    return restored;
}

void validate_bounded_verification_retraction(
    const CognitiveState& state, const BoundedWorldWriteReceipt& receipt) {
    if (receipt.target_role != "verification")
        throw std::invalid_argument("receipt target role is invalid");
    const auto topology = verification_topology(state);
    const auto found = state.self_state().find(std::string(write_key));
    if (found == state.self_state().end() || !found->second.is_object())
        throw std::invalid_argument("bounded write receipt is no longer active");
    const auto& current = found->second.as_object();
    const auto receipt_id_value = current.find("receipt_id");
    const auto revision_value = current.find("revision");
    const auto target_value = current.find("target_role");
    bool head_matches = receipt_id_value != current.end() &&
        revision_value != current.end() && target_value != current.end();
    if (head_matches) {
        try {
            head_matches = receipt_id_value->second.as_string() == receipt.receipt_id &&
                exact_json_integer_equal(revision_value->second, receipt.revision) &&
                target_value->second.as_string() == receipt.target_role;
        } catch (const std::invalid_argument&) {
            head_matches = false;
        }
    }
    if (!head_matches)
        throw std::invalid_argument("bounded write receipt is not the current LIFO head");
    const auto local_index = topology.local_index;
    if (tensor_hash(slot_tensor(state, local_index)) != receipt.after_slot_hash)
        throw std::invalid_argument("verification slot has a newer or unrelated value");
}

std::shared_ptr<const CognitiveState> retract_bounded_verification_write(
    std::shared_ptr<const CognitiveState> state,
    const BoundedWorldWriteReceipt& receipt) {
    if (!state) throw std::invalid_argument("bounded retraction state must not be null");
    validate_bounded_verification_retraction(*state, receipt);
    const auto local_index = verification_topology(*state).local_index;
    const auto current = slot_tensor(*state, local_index);
    const Tensor before_slot(
        current.dtype(), std::vector<std::uint64_t>(receipt.before_slot.shape().begin(),
                                                    receipt.before_slot.shape().end()),
        std::vector<double>(receipt.before_slot.values().begin(),
                            receipt.before_slot.values().end()),
        std::string(current.device()));
    if (tensor_hash(before_slot) != receipt.before_slot_hash)
        throw std::invalid_argument("receipt before slot differs from its content hash");
    auto self_state = state->self_state();
    if (receipt.prior_write_metadata) {
        self_state[std::string(write_key)] = JsonValue(*receipt.prior_write_metadata);
    } else {
        self_state.erase(std::string(write_key));
    }
    return replace_scratch_and_self(
        *state, replace_scratch_slot(state->scratch_slots(), local_index, before_slot),
        std::move(self_state));
}

}  // namespace swegca::world
