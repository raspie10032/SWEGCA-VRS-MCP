#include "world/cognitive_state.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace swegca::world {
namespace {

[[nodiscard]] bool has_text(const std::string_view value) noexcept {
    for (const unsigned char c : value) {
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '\f' && c != '\v') {
            return true;
        }
    }
    return false;
}

void require_text(const std::string_view value, const char* name) {
    if (!has_text(value)) {
        throw std::invalid_argument(std::string(name) + " must not be empty");
    }
}

[[nodiscard]] std::uint64_t checked_numel(const std::span<const std::uint64_t> shape) {
    std::uint64_t result = 1;
    for (const auto dimension : shape) {
        if (dimension != 0 && result > std::numeric_limits<std::uint64_t>::max() / dimension) {
            throw std::overflow_error("tensor element count overflow");
        }
        result *= dimension;
    }
    return result;
}

[[nodiscard]] std::string_view dtype_name(const TensorDType dtype) noexcept {
    switch (dtype) {
    case TensorDType::bfloat16:
        return "bfloat16";
    case TensorDType::float16:
        return "float16";
    case TensorDType::float32:
        return "float32";
    case TensorDType::float64:
        return "float64";
    }
    return {};
}

[[nodiscard]] TensorDType parse_dtype(const std::string_view name) {
    if (name == "bfloat16") {
        return TensorDType::bfloat16;
    }
    if (name == "float16") {
        return TensorDType::float16;
    }
    if (name == "float32") {
        return TensorDType::float32;
    }
    if (name == "float64") {
        return TensorDType::float64;
    }
    throw std::invalid_argument("unsupported cognitive state dtype: " + std::string(name));
}

[[nodiscard]] std::uint64_t round_shift_right_to_even(
    const std::uint64_t value, const unsigned shift) noexcept {
    if (shift == 0) return value;
    if (shift >= 64) return 0;
    const std::uint64_t quotient = value >> shift;
    const std::uint64_t remainder = value & ((std::uint64_t{1} << shift) - 1U);
    const std::uint64_t halfway = std::uint64_t{1} << (shift - 1U);
    return quotient + static_cast<std::uint64_t>(
        remainder > halfway || (remainder == halfway && (quotient & 1U) != 0));
}

[[nodiscard]] std::uint16_t binary64_to_reduced_bits(
    const double value, const unsigned exponent_bits, const unsigned fraction_bits,
    const int exponent_bias) noexcept {
    const std::uint64_t raw = std::bit_cast<std::uint64_t>(value);
    const std::uint16_t sign = static_cast<std::uint16_t>(
        (raw >> 63U) << (exponent_bits + fraction_bits));
    const std::uint64_t source_exponent = (raw >> 52U) & 0x7ffU;
    const std::uint64_t source_fraction = raw & ((std::uint64_t{1} << 52U) - 1U);
    const std::uint16_t target_exponent_mask = static_cast<std::uint16_t>(
        (std::uint16_t{1} << exponent_bits) - 1U);
    const auto encode = [&](const std::uint16_t exponent,
                            const std::uint16_t fraction) noexcept {
        return static_cast<std::uint16_t>(
            sign | static_cast<std::uint16_t>(exponent << fraction_bits) | fraction);
    };

    if (source_exponent == 0x7ffU) {
        if (source_fraction == 0) return encode(target_exponent_mask, 0);
        return encode(target_exponent_mask,
                      static_cast<std::uint16_t>(std::uint16_t{1} <<
                                                 (fraction_bits - 1U)));
    }
    if (source_exponent == 0 && source_fraction == 0) return sign;

    const std::uint64_t significand = source_exponent == 0
        ? source_fraction
        : (std::uint64_t{1} << 52U) | source_fraction;
    const int source_power = source_exponent == 0
        ? -1074
        : static_cast<int>(source_exponent) - 1023 - 52;
    const unsigned highest_bit = 63U - static_cast<unsigned>(std::countl_zero(significand));
    int target_exponent = source_power + static_cast<int>(highest_bit);
    const int minimum_normal_exponent = 1 - exponent_bias;
    const int maximum_normal_exponent =
        static_cast<int>(target_exponent_mask) - 1 - exponent_bias;

    if (target_exponent < minimum_normal_exponent) {
        const int subnormal_power = minimum_normal_exponent -
            static_cast<int>(fraction_bits);
        const int shift = subnormal_power - source_power;
        const std::uint64_t fraction = shift > 0
            ? round_shift_right_to_even(significand, static_cast<unsigned>(shift))
            : significand << static_cast<unsigned>(-shift);
        if (fraction == 0) return sign;
        if (fraction >= (std::uint64_t{1} << fraction_bits)) return encode(1, 0);
        return encode(0, static_cast<std::uint16_t>(fraction));
    }
    if (target_exponent > maximum_normal_exponent) {
        return encode(target_exponent_mask, 0);
    }

    const unsigned precision = fraction_bits + 1U;
    const unsigned shift = highest_bit - (precision - 1U);
    std::uint64_t rounded = round_shift_right_to_even(significand, shift);
    if (rounded == (std::uint64_t{1} << precision)) {
        rounded >>= 1U;
        ++target_exponent;
        if (target_exponent > maximum_normal_exponent) {
            return encode(target_exponent_mask, 0);
        }
    }
    const auto stored_exponent = static_cast<std::uint16_t>(target_exponent + exponent_bias);
    const auto stored_fraction = static_cast<std::uint16_t>(
        rounded - (std::uint64_t{1} << fraction_bits));
    return encode(stored_exponent, stored_fraction);
}

[[nodiscard]] double reduced_bits_to_binary64(
    const std::uint16_t bits, const unsigned exponent_bits,
    const unsigned fraction_bits, const int exponent_bias) noexcept {
    const std::uint16_t fraction_mask = static_cast<std::uint16_t>(
        (std::uint16_t{1} << fraction_bits) - 1U);
    const std::uint16_t exponent_mask = static_cast<std::uint16_t>(
        (std::uint16_t{1} << exponent_bits) - 1U);
    const bool negative = (bits & static_cast<std::uint16_t>(
        std::uint16_t{1} << (exponent_bits + fraction_bits))) != 0;
    const std::uint16_t exponent = static_cast<std::uint16_t>(
        (bits >> fraction_bits) & exponent_mask);
    const std::uint16_t fraction = static_cast<std::uint16_t>(bits & fraction_mask);

    double result = 0.0;
    if (exponent == exponent_mask) {
        result = fraction == 0 ? std::numeric_limits<double>::infinity()
                               : std::numeric_limits<double>::quiet_NaN();
    } else if (exponent == 0) {
        result = std::ldexp(static_cast<double>(fraction),
                            1 - exponent_bias - static_cast<int>(fraction_bits));
    } else {
        result = std::ldexp(
            1.0 + std::ldexp(static_cast<double>(fraction),
                             -static_cast<int>(fraction_bits)),
            static_cast<int>(exponent) - exponent_bias);
    }
    return std::copysign(result, negative ? -1.0 : 1.0);
}

[[nodiscard]] double canonical_tensor_value(
    const TensorDType dtype, const double value) noexcept {
    switch (dtype) {
    case TensorDType::float64:
        return value;
    case TensorDType::float32:
        return static_cast<double>(static_cast<float>(value));
    case TensorDType::bfloat16:
        return reduced_bits_to_binary64(
            binary64_to_reduced_bits(value, 8, 7, 127), 8, 7, 127);
    case TensorDType::float16:
        return reduced_bits_to_binary64(
            binary64_to_reduced_bits(value, 5, 10, 15), 5, 10, 15);
    }
    return value;
}

[[nodiscard]] JsonValue nested_values(const std::span<const std::uint64_t> shape,
                                      const std::span<const double> values,
                                      const std::size_t dimension, std::size_t& cursor) {
    if (dimension == shape.size()) {
        if (cursor >= values.size()) {
            throw std::logic_error("tensor value cursor exceeded storage");
        }
        return JsonValue(values[cursor++]);
    }
    JsonValue::Array result;
    if (shape[dimension] > result.max_size()) {
        throw std::length_error("tensor dimension exceeds JSON array capacity");
    }
    result.reserve(static_cast<std::size_t>(shape[dimension]));
    for (std::uint64_t index = 0; index < shape[dimension]; ++index) {
        result.push_back(nested_values(shape, values, dimension + 1, cursor));
    }
    return JsonValue(std::move(result));
}

struct ParsedValues final {
    std::vector<std::uint64_t> shape;
    std::vector<double> values;
};

[[nodiscard]] ParsedValues parse_values(const JsonValue& value) {
    if (!value.is_array()) {
        return {{}, {value.as_number()}};
    }
    const auto& array = value.as_array();
    ParsedValues result;
    result.shape.push_back(array.size());
    if (array.empty()) {
        return result;
    }
    auto first = parse_values(array.front());
    result.shape.insert(result.shape.end(), first.shape.begin(), first.shape.end());
    result.values = std::move(first.values);
    for (std::size_t index = 1; index < array.size(); ++index) {
        auto child = parse_values(array[index]);
        if (child.shape.size() + 1 != result.shape.size() ||
            !std::equal(child.shape.begin(), child.shape.end(), result.shape.begin() + 1)) {
            throw std::invalid_argument("tensor values must form a rectangular array");
        }
        result.values.insert(result.values.end(), child.values.begin(), child.values.end());
    }
    return result;
}

[[nodiscard]] const JsonValue* optional(const JsonValue::Object& object,
                                        const std::string_view key) noexcept {
    const auto found = object.find(key);
    return found == object.end() ? nullptr : &found->second;
}

[[nodiscard]] std::vector<std::string> string_array(const JsonValue& value) {
    std::vector<std::string> result;
    result.reserve(value.as_array().size());
    for (const auto& item : value.as_array()) {
        result.emplace_back(item.as_string());
    }
    return result;
}

[[nodiscard]] JsonValue json_strings(const std::span<const std::string> values) {
    JsonValue::Array result;
    result.reserve(values.size());
    for (const auto& value : values) {
        result.emplace_back(value);
    }
    return JsonValue(std::move(result));
}

void reject_unknown_keys(const JsonValue::Object& object,
                         const std::span<const std::string_view> allowed) {
    for (const auto& [key, value] : object) {
        static_cast<void>(value);
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) {
            throw std::invalid_argument("unexpected object field: " + key);
        }
    }
}

}  // namespace

JsonValue::JsonValue() noexcept : storage_(nullptr) {}
JsonValue::JsonValue(std::nullptr_t) noexcept : storage_(nullptr) {}
JsonValue::JsonValue(const bool value) noexcept : storage_(value) {}
JsonValue::JsonValue(const std::int64_t value) noexcept : storage_(value) {}
JsonValue::JsonValue(const int value) noexcept : storage_(static_cast<std::int64_t>(value)) {}
JsonValue::JsonValue(const double value) noexcept : storage_(value) {}
JsonValue::JsonValue(std::string value) : storage_(std::move(value)) {}
JsonValue::JsonValue(const std::string_view value) : storage_(std::string(value)) {}
JsonValue::JsonValue(const char* value) : storage_(std::string(value)) {}
JsonValue::JsonValue(Array value) : storage_(std::move(value)) {}
JsonValue::JsonValue(Object value) : storage_(std::move(value)) {}

bool JsonValue::is_array() const noexcept { return std::holds_alternative<Array>(storage_); }
bool JsonValue::is_object() const noexcept { return std::holds_alternative<Object>(storage_); }

const JsonValue::Array& JsonValue::as_array() const {
    if (!is_array()) {
        throw std::invalid_argument("JSON value is not an array");
    }
    return std::get<Array>(storage_);
}

const JsonValue::Object& JsonValue::as_object() const {
    if (!is_object()) {
        throw std::invalid_argument("JSON value is not an object");
    }
    return std::get<Object>(storage_);
}

std::string_view JsonValue::as_string() const {
    if (!std::holds_alternative<std::string>(storage_)) {
        throw std::invalid_argument("JSON value is not a string");
    }
    return std::get<std::string>(storage_);
}

double JsonValue::as_number() const {
    if (std::holds_alternative<double>(storage_)) {
        return std::get<double>(storage_);
    }
    if (std::holds_alternative<std::int64_t>(storage_)) {
        return static_cast<double>(std::get<std::int64_t>(storage_));
    }
    if (std::holds_alternative<bool>(storage_)) {
        return std::get<bool>(storage_) ? 1.0 : 0.0;
    }
    throw std::invalid_argument("JSON value is not numeric");
}

const JsonValue& JsonValue::at(const std::string_view key) const {
    const auto& object = as_object();
    const auto found = object.find(key);
    if (found == object.end()) {
        throw std::out_of_range("missing JSON field: " + std::string(key));
    }
    return found->second;
}

const JsonValue::Storage& JsonValue::storage() const noexcept { return storage_; }

Tensor::Tensor(const TensorDType dtype, std::vector<std::uint64_t> shape,
               std::vector<double> values, std::string device)
    : dtype_(dtype), shape_(std::move(shape)),
      values_(std::make_shared<std::vector<double>>(std::move(values))),
      device_(std::move(device)) {
    if (dtype_name(dtype_).empty()) {
        throw std::invalid_argument("unsupported cognitive state dtype");
    }
    require_text(device_, "device");
    const auto count = checked_numel(shape_);
    if (count > std::numeric_limits<std::size_t>::max() ||
        static_cast<std::size_t>(count) != values_->size()) {
        throw std::invalid_argument("tensor shape does not match value count");
    }
    for (double& value : *values_) value = canonical_tensor_value(dtype_, value);
}

TensorDType Tensor::dtype() const noexcept { return dtype_; }
std::span<const std::uint64_t> Tensor::shape() const noexcept { return shape_; }
std::span<const double> Tensor::values() const noexcept { return *values_; }
std::string_view Tensor::device() const noexcept { return device_; }
std::size_t Tensor::rank() const noexcept { return shape_.size(); }
const void* Tensor::storage_identity() const noexcept { return values_.get(); }
Tensor Tensor::clone() const {
    return Tensor(dtype_, shape_, std::vector<double>(values_->begin(), values_->end()), device_);
}

bool Tensor::exact_equal(const Tensor& other) const noexcept {
    if (dtype_ != other.dtype_ || shape_ != other.shape_ || device_ != other.device_ ||
        values_->size() != other.values_->size()) {
        return false;
    }
    for (std::size_t index = 0; index < values_->size(); ++index) {
        if (std::bit_cast<std::uint64_t>((*values_)[index]) !=
            std::bit_cast<std::uint64_t>((*other.values_)[index])) {
            return false;
        }
    }
    return true;
}

JsonValue Tensor::to_json_payload() const {
    std::size_t cursor = 0;
    auto values = nested_values(shape_, *values_, 0, cursor);
    if (cursor != values_->size()) {
        throw std::logic_error("tensor serialization did not consume every value");
    }
    return JsonValue::Object{{"dtype", dtype_name(dtype_)}, {"values", std::move(values)}};
}

Tensor Tensor::from_json_payload(const JsonValue& payload) {
    static_cast<void>(payload.as_object());
    auto parsed = parse_values(payload.at("values"));
    return Tensor(parse_dtype(payload.at("dtype").as_string()), std::move(parsed.shape),
                  std::move(parsed.values));
}

WorldEntity::WorldEntity(std::string id, std::string type, JsonValue::Object properties_value,
                         JsonValue::Object spatial_value, std::vector<std::string> refs)
    : entity_id(std::move(id)), entity_type(std::move(type)),
      properties(std::move(properties_value)), spatial(std::move(spatial_value)),
      evidence_refs(std::move(refs)) {
    require_text(entity_id, "entity_id");
    require_text(entity_type, "entity_type");
    for (const auto& reference : evidence_refs) {
        if (reference.empty()) {
            throw std::invalid_argument("entity evidence_refs must not contain empty addresses");
        }
    }
}

JsonValue WorldEntity::to_json() const {
    return JsonValue::Object{{"entity_id", entity_id},
                             {"entity_type", entity_type},
                             {"properties", properties},
                             {"spatial", spatial},
                             {"evidence_refs", json_strings(evidence_refs)}};
}

WorldEntity WorldEntity::from_json(const JsonValue& payload) {
    const auto& object = payload.as_object();
    constexpr std::string_view allowed[]{"entity_id", "entity_type", "properties", "spatial",
                                         "evidence_refs"};
    reject_unknown_keys(object, allowed);
    return WorldEntity(std::string(payload.at("entity_id").as_string()),
                       std::string(payload.at("entity_type").as_string()),
                       optional(object, "properties")
                           ? optional(object, "properties")->as_object()
                           : JsonValue::Object{},
                       optional(object, "spatial") ? optional(object, "spatial")->as_object()
                                                    : JsonValue::Object{},
                       optional(object, "evidence_refs")
                           ? string_array(*optional(object, "evidence_refs"))
                           : std::vector<std::string>{});
}

WorldRelation::WorldRelation(std::string subject_value, std::string predicate_value,
                             std::string object_value, JsonValue::Object properties_value)
    : subject(std::move(subject_value)), predicate(std::move(predicate_value)),
      object(std::move(object_value)), properties(std::move(properties_value)) {
    require_text(subject, "subject");
    require_text(predicate, "predicate");
    require_text(object, "object");
}

JsonValue WorldRelation::to_json() const {
    return JsonValue::Object{{"subject", subject},
                             {"predicate", predicate},
                             {"object", object},
                             {"properties", properties}};
}

WorldRelation WorldRelation::from_json(const JsonValue& payload) {
    const auto& object_value = payload.as_object();
    constexpr std::string_view allowed[]{"subject", "predicate", "object", "properties"};
    reject_unknown_keys(object_value, allowed);
    return WorldRelation(std::string(payload.at("subject").as_string()),
                         std::string(payload.at("predicate").as_string()),
                         std::string(payload.at("object").as_string()),
                         optional(object_value, "properties")
                             ? optional(object_value, "properties")->as_object()
                             : JsonValue::Object{});
}

StructuredWorldGraph::StructuredWorldGraph(std::vector<WorldEntity> entities,
                                           std::vector<WorldRelation> relations)
    : entities_(std::move(entities)), relations_(std::move(relations)) {
    std::set<std::string, std::less<>> identifiers;
    for (const auto& entity : entities_) {
        if (!identifiers.insert(entity.entity_id).second) {
            throw std::invalid_argument("entity IDs must be unique");
        }
    }
    for (const auto& relation : relations_) {
        if (!identifiers.contains(relation.subject) || !identifiers.contains(relation.object)) {
            throw std::invalid_argument("relations must not reference unknown entities");
        }
    }
}

std::span<const WorldEntity> StructuredWorldGraph::entities() const noexcept { return entities_; }
std::span<const WorldRelation> StructuredWorldGraph::relations() const noexcept {
    return relations_;
}

JsonValue StructuredWorldGraph::to_json() const {
    JsonValue::Array entities;
    JsonValue::Array relations;
    entities.reserve(entities_.size());
    relations.reserve(relations_.size());
    for (const auto& entity : entities_) {
        entities.push_back(entity.to_json());
    }
    for (const auto& relation : relations_) {
        relations.push_back(relation.to_json());
    }
    return JsonValue::Object{{"entities", std::move(entities)},
                             {"relations", std::move(relations)}};
}

StructuredWorldGraph StructuredWorldGraph::from_json(const JsonValue& payload) {
    const auto& object = payload.as_object();
    std::vector<WorldEntity> entities;
    std::vector<WorldRelation> relations;
    if (const auto* values = optional(object, "entities")) {
        entities.reserve(values->as_array().size());
        for (const auto& value : values->as_array()) {
            entities.push_back(WorldEntity::from_json(value));
        }
    }
    if (const auto* values = optional(object, "relations")) {
        relations.reserve(values->as_array().size());
        for (const auto& value : values->as_array()) {
            relations.push_back(WorldRelation::from_json(value));
        }
    }
    return StructuredWorldGraph(std::move(entities), std::move(relations));
}

void CognitiveKernelConfig::validate() const {
    if (semantic_slots == 0 || executive_slots == 0 || scratch_slots == 0 || hidden_dim == 0) {
        throw std::invalid_argument("slot counts and hidden_dim must be positive");
    }
}

CognitiveState::CognitiveState(Tensor semantic, Tensor executive, Tensor scratch,
                               StructuredWorldGraph graph, std::vector<std::string> references,
                               JsonValue::Object goal, JsonValue::Object value,
                               JsonValue::Object self, std::string owner)
    : semantic_slots_(std::move(semantic)), executive_slots_(std::move(executive)),
      scratch_slots_(std::move(scratch)), structured_world_graph_(std::move(graph)),
      evidence_refs_(std::move(references)), goal_state_(std::move(goal)),
      value_state_(std::move(value)), self_state_(std::move(self)), owner_id_(std::move(owner)) {
    require_text(owner_id_, "owner_id");
    const Tensor* tensors[]{&semantic_slots_, &executive_slots_, &scratch_slots_};
    for (const auto* tensor : tensors) {
        if (tensor->rank() != 3) {
            throw std::invalid_argument(
                "cognitive slot tensors must have shape [batch, slots, dim]");
        }
    }
    const auto semantic_shape = semantic_slots_.shape();
    for (const auto* tensor : {&executive_slots_, &scratch_slots_}) {
        const auto shape = tensor->shape();
        if (shape[0] != semantic_shape[0] || shape[2] != semantic_shape[2]) {
            throw std::invalid_argument(
                "all cognitive slot tensors must share batch and hidden dims");
        }
        if (tensor->dtype() != semantic_slots_.dtype()) {
            throw std::invalid_argument("all cognitive slot tensors must share dtype");
        }
        if (tensor->device() != semantic_slots_.device()) {
            throw std::invalid_argument("all cognitive slot tensors must share device");
        }
    }
}

const Tensor& CognitiveState::semantic_slots() const noexcept { return semantic_slots_; }
const Tensor& CognitiveState::executive_slots() const noexcept { return executive_slots_; }
const Tensor& CognitiveState::scratch_slots() const noexcept { return scratch_slots_; }
const StructuredWorldGraph& CognitiveState::structured_world_graph() const noexcept {
    return structured_world_graph_;
}
std::span<const std::string> CognitiveState::evidence_refs() const noexcept {
    return evidence_refs_;
}
const JsonValue::Object& CognitiveState::goal_state() const noexcept { return goal_state_; }
const JsonValue::Object& CognitiveState::value_state() const noexcept { return value_state_; }
const JsonValue::Object& CognitiveState::self_state() const noexcept { return self_state_; }
std::string_view CognitiveState::owner_id() const noexcept { return owner_id_; }

void CognitiveState::validate(const CognitiveKernelConfig& config) const {
    config.validate();
    const auto semantic = semantic_slots_.shape();
    const auto executive = executive_slots_.shape();
    const auto scratch = scratch_slots_.shape();
    if (semantic[1] != config.semantic_slots || semantic[2] != config.hidden_dim ||
        executive[1] != config.executive_slots || executive[2] != config.hidden_dim ||
        scratch[1] != config.scratch_slots || scratch[2] != config.hidden_dim) {
        throw std::invalid_argument("unexpected cognitive slot shapes");
    }
}

CognitiveState CognitiveState::with_metadata(JsonValue::Object goal,
                                             JsonValue::Object self) const {
    return CognitiveState(semantic_slots_, executive_slots_, scratch_slots_,
                          structured_world_graph_, evidence_refs_, std::move(goal),
                          value_state_, std::move(self), owner_id_);
}

CognitiveState CognitiveState::clone() const {
    return CognitiveState(semantic_slots_.clone(), executive_slots_.clone(), scratch_slots_.clone(),
                          structured_world_graph_, evidence_refs_, goal_state_, value_state_,
                          self_state_, owner_id_);
}

bool CognitiveState::exact_equal(const CognitiveState& other) const noexcept {
    return semantic_slots_.exact_equal(other.semantic_slots_) &&
           executive_slots_.exact_equal(other.executive_slots_) &&
           scratch_slots_.exact_equal(other.scratch_slots_) &&
           structured_world_graph_ == other.structured_world_graph_ &&
           evidence_refs_ == other.evidence_refs_ && goal_state_ == other.goal_state_ &&
           value_state_ == other.value_state_ && self_state_ == other.self_state_ &&
           owner_id_ == other.owner_id_;
}

JsonValue CognitiveState::to_dict() const {
    return JsonValue::Object{{"semantic_slots", semantic_slots_.to_json_payload()},
                             {"executive_slots", executive_slots_.to_json_payload()},
                             {"scratch_slots", scratch_slots_.to_json_payload()},
                             {"structured_world_graph", structured_world_graph_.to_json()},
                             {"evidence_refs", json_strings(evidence_refs_)},
                             {"goal_state", goal_state_},
                             {"value_state", value_state_},
                             {"self_state", self_state_},
                             {"owner_id", owner_id_}};
}

CognitiveState CognitiveState::from_dict(const JsonValue& payload) {
    const auto& object = payload.as_object();
    return CognitiveState(
        Tensor::from_json_payload(payload.at("semantic_slots")),
        Tensor::from_json_payload(payload.at("executive_slots")),
        Tensor::from_json_payload(payload.at("scratch_slots")),
        optional(object, "structured_world_graph")
            ? StructuredWorldGraph::from_json(*optional(object, "structured_world_graph"))
            : StructuredWorldGraph{},
        optional(object, "evidence_refs") ? string_array(*optional(object, "evidence_refs"))
                                           : std::vector<std::string>{},
        optional(object, "goal_state") ? optional(object, "goal_state")->as_object()
                                        : JsonValue::Object{},
        optional(object, "value_state") ? optional(object, "value_state")->as_object()
                                         : JsonValue::Object{},
        optional(object, "self_state") ? optional(object, "self_state")->as_object()
                                        : JsonValue::Object{},
        optional(object, "owner_id") ? std::string(optional(object, "owner_id")->as_string())
                                      : std::string(default_owner));
}

}  // namespace swegca::world
