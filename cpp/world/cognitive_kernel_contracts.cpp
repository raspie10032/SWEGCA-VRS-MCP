#include "world/cognitive_kernel_contracts.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>

namespace swegca::world {
namespace {

void text(const std::string_view value, const char* label) {
    if (value.empty()) throw std::invalid_argument(std::string(label) + " must not be empty");
}

const JsonValue::Object& object(const JsonValue& value) { return value.as_object(); }
std::string member_text(const JsonValue::Object& value, const std::string_view key) {
    return std::string(value.at(std::string(key)).as_string());
}
bool member_bool(const JsonValue::Object& value, const std::string_view key) {
    const auto* result = std::get_if<bool>(&value.at(std::string(key)).storage());
    if (!result) throw std::invalid_argument("expected boolean member");
    return *result;
}
std::optional<std::string> optional_text(const JsonValue::Object& value,
                                         const std::string_view key) {
    const auto found = value.find(key);
    if (found == value.end() || std::holds_alternative<std::nullptr_t>(found->second.storage()))
        return std::nullopt;
    return std::string(found->second.as_string());
}
JsonValue::Array string_array(const std::vector<std::string>& values) {
    JsonValue::Array result;
    for (const auto& value : values) result.emplace_back(value);
    return result;
}
std::vector<std::string> strings(const JsonValue& value) {
    std::vector<std::string> result;
    for (const auto& row : value.as_array()) result.emplace_back(row.as_string());
    return result;
}
std::uint64_t checked_add(const std::uint64_t left, const std::uint64_t right) {
    if (left > std::numeric_limits<std::uint64_t>::max() - right)
        throw std::overflow_error("resource estimate overflow");
    return left + right;
}
std::uint64_t checked_mul(const std::uint64_t left, const std::uint64_t right) {
    if (left && right > std::numeric_limits<std::uint64_t>::max() / left)
        throw std::overflow_error("resource estimate overflow");
    return left * right;
}

}  // namespace

RepresentationRegistryEntry::RepresentationRegistryEntry(
    std::string id_value, std::string type_value, JsonValue::Array shape,
    std::string encoder_value, std::optional<std::string> decoder_value,
    std::string bridge_in_value, std::optional<std::string> bridge_out_value,
    JsonValue::Object requirements, std::string precision_value,
    std::string license_value, std::vector<std::string> capabilities_value)
    : id(std::move(id_value)), type(std::move(type_value)), native_shape(std::move(shape)),
      encoder(std::move(encoder_value)), decoder(std::move(decoder_value)),
      bridge_in(std::move(bridge_in_value)), bridge_out(std::move(bridge_out_value)),
      device_requirements(std::move(requirements)), precision(std::move(precision_value)),
      license(std::move(license_value)), capabilities(std::move(capabilities_value)) {
    text(id, "id"); text(type, "type"); text(encoder, "encoder");
    text(bridge_in, "bridge_in"); text(precision, "precision"); text(license, "license");
    if (native_shape.empty() || capabilities.empty())
        throw std::invalid_argument("native_shape and capabilities must not be empty");
    for (const auto& row : native_shape)
        if (!std::holds_alternative<std::int64_t>(row.storage()) &&
            !std::holds_alternative<std::string>(row.storage()))
            throw std::invalid_argument("native_shape entries must be integers or text");
    for (const auto& value : capabilities) text(value, "capability");
}

JsonValue RepresentationRegistryEntry::to_dict() const {
    return JsonValue::Object{{"id", id}, {"type", type}, {"native_shape", native_shape},
        {"encoder", encoder}, {"decoder", decoder ? JsonValue(*decoder) : JsonValue(nullptr)},
        {"bridge_in", bridge_in},
        {"bridge_out", bridge_out ? JsonValue(*bridge_out) : JsonValue(nullptr)},
        {"device_requirements", device_requirements}, {"precision", precision},
        {"license", license}, {"capabilities", string_array(capabilities)}};
}

RepresentationRegistryEntry RepresentationRegistryEntry::from_dict(const JsonValue& value) {
    const auto& rows = object(value);
    return {member_text(rows, "id"), member_text(rows, "type"),
        rows.at("native_shape").as_array(), member_text(rows, "encoder"),
        optional_text(rows, "decoder"), member_text(rows, "bridge_in"),
        optional_text(rows, "bridge_out"), rows.at("device_requirements").as_object(),
        member_text(rows, "precision"), member_text(rows, "license"),
        strings(rows.at("capabilities"))};
}

PhysicalQuery::PhysicalQuery(std::string id, std::string type,
    JsonValue::Object origin_value, std::string scene,
    JsonValue::Object requirements_value)
    : query_id(std::move(id)), query_type(std::move(type)), origin(std::move(origin_value)),
      scene_ref(std::move(scene)), requirements(std::move(requirements_value)) {
    text(query_id, "query_id"); text(query_type, "query_type"); text(scene_ref, "scene_ref");
}

JsonValue PhysicalQuery::to_dict() const {
    return JsonValue::Object{{"query_id", query_id}, {"query_type", query_type},
        {"origin", origin}, {"scene_ref", scene_ref}, {"requirements", requirements}};
}

PhysicalQuery PhysicalQuery::from_dict(const JsonValue& value) {
    const auto& rows = object(value);
    return {member_text(rows, "query_id"), member_text(rows, "query_type"),
        rows.at("origin").as_object(), member_text(rows, "scene_ref"),
        rows.at("requirements").as_object()};
}

PhysicalEvidence::PhysicalEvidence(std::string event, std::string query,
    std::string type, std::vector<EvidenceClaim> claims_value,
    std::string backend_value, std::string scene, const bool deterministic_value)
    : event_id(std::move(event)), query_id(std::move(query)), evidence_type(std::move(type)),
      claims(std::move(claims_value)), backend(std::move(backend_value)),
      scene_ref(std::move(scene)), deterministic(deterministic_value) {
    text(event_id, "event_id"); text(query_id, "query_id"); text(evidence_type, "evidence_type");
    text(backend, "backend"); text(scene_ref, "scene_ref");
    if (!deterministic) throw std::invalid_argument("PhysicalEvidence requires a deterministic backend result");
}

JsonValue PhysicalEvidence::to_dict() const {
    JsonValue::Array rows;
    for (const auto& claim : claims) rows.push_back(claim.to_dict());
    return JsonValue::Object{{"event_id", event_id}, {"query_id", query_id},
        {"evidence_type", evidence_type}, {"claims", std::move(rows)},
        {"backend", backend}, {"scene_ref", scene_ref}, {"deterministic", deterministic}};
}

PhysicalEvidence PhysicalEvidence::from_dict(const JsonValue& value) {
    const auto& rows = object(value);
    std::vector<EvidenceClaim> claims;
    for (const auto& claim : rows.at("claims").as_array())
        claims.push_back(EvidenceClaim::from_dict(claim));
    return {member_text(rows, "event_id"), member_text(rows, "query_id"),
        member_text(rows, "evidence_type"), std::move(claims),
        member_text(rows, "backend"), member_text(rows, "scene_ref"),
        member_bool(rows, "deterministic")};
}

WorldBundle::WorldBundle(std::string id, std::vector<std::string> entities,
    std::map<std::string, std::vector<std::string>, std::less<>> representations_value,
    JsonValue::Object metadata, JsonValue::Object provenance_value,
    std::string license_value)
    : bundle_id(std::move(id)), entity_ids(std::move(entities)),
      representations(std::move(representations_value)),
      entity_relation_metadata(std::move(metadata)), provenance(std::move(provenance_value)),
      license(std::move(license_value)) {
    text(bundle_id, "bundle_id"); text(license, "license");
    std::set<std::string, std::less<>> unique(entity_ids.begin(), entity_ids.end());
    if (entity_ids.empty() || unique.size() != entity_ids.size() || representations.empty())
        throw std::invalid_argument("world bundle identities or representations changed");
    for (const auto& [kind, refs] : representations) {
        text(kind, "representation kind");
        if (refs.empty()) throw std::invalid_argument("representation references must not be empty");
        for (const auto& ref : refs) text(ref, "representation reference");
    }
}

JsonValue WorldBundle::to_dict() const {
    JsonValue::Object representation_rows;
    for (const auto& [kind, refs] : representations)
        representation_rows.emplace(kind, string_array(refs));
    return JsonValue::Object{{"bundle_id", bundle_id}, {"entity_ids", string_array(entity_ids)},
        {"representations", std::move(representation_rows)},
        {"entity_relation_metadata", entity_relation_metadata}, {"provenance", provenance},
        {"license", license}};
}

WorldBundle WorldBundle::from_dict(const JsonValue& value) {
    const auto& rows = object(value);
    std::map<std::string, std::vector<std::string>, std::less<>> representations;
    for (const auto& [kind, refs] : rows.at("representations").as_object())
        representations.emplace(kind, strings(refs));
    return {member_text(rows, "bundle_id"), strings(rows.at("entity_ids")),
        std::move(representations), rows.at("entity_relation_metadata").as_object(),
        rows.at("provenance").as_object(), member_text(rows, "license")};
}

void RecurrentCoreSpec::validate() const {
    state.validate();
    if (!hidden_dim || !unique_blocks || !attention_heads || !kv_heads ||
        !mlp_hidden_dim || !max_cycles)
        throw std::invalid_argument("recurrent core dimensions must be positive");
    if (hidden_dim != state.hidden_dim)
        throw std::invalid_argument("core and cognitive state hidden dimensions must match");
    if (hidden_dim % attention_heads || attention_heads % kv_heads)
        throw std::invalid_argument("recurrent attention dimensions changed");
}

void VramEstimateConfig::validate() const {
    if (!(weight_bits > 0) || !(activation_multiplier > 0) || !activation_bytes || !batch_size)
        throw std::invalid_argument("VRAM estimate values changed");
}

RecurrentCoreParameterEstimate estimate_recurrent_core_parameters(
    const RecurrentCoreSpec& spec) {
    spec.validate();
    const auto head_dim = spec.hidden_dim / spec.attention_heads;
    const auto kv_dim = checked_mul(head_dim, spec.kv_heads);
    const auto attention = checked_add(checked_mul(checked_mul(spec.hidden_dim, spec.hidden_dim), 2),
        checked_mul(checked_mul(spec.hidden_dim, kv_dim), 2));
    const auto mlp = checked_mul(checked_mul(3, spec.hidden_dim), spec.mlp_hidden_dim);
    const auto norm = checked_mul(2, spec.hidden_dim);
    const auto block = checked_add(checked_add(attention, mlp), norm);
    const auto total_slots = checked_add(checked_add(spec.state.semantic_slots,
        spec.state.executive_slots), spec.state.scratch_slots);
    const auto state_embeddings = checked_mul(total_slots, spec.hidden_dim);
    const auto total = checked_add(checked_add(checked_mul(spec.unique_blocks, block),
        state_embeddings), spec.hidden_dim);
    return {attention, mlp, norm, block, state_embeddings, spec.hidden_dim, total};
}

InferenceVramEstimate estimate_inference_vram(
    const RecurrentCoreSpec& spec, const VramEstimateConfig& config) {
    spec.validate(); config.validate();
    const auto parameters = estimate_recurrent_core_parameters(spec).total_parameters;
    const auto weights = static_cast<std::uint64_t>(
        std::ceil(static_cast<long double>(parameters) * config.weight_bits / 8.0L));
    const auto slots = checked_add(checked_add(spec.state.semantic_slots,
        spec.state.executive_slots), spec.state.scratch_slots);
    const auto state = checked_mul(checked_mul(checked_mul(config.batch_size, slots),
        spec.hidden_dim), config.activation_bytes);
    const auto head_dim = spec.hidden_dim / spec.attention_heads;
    auto kv = checked_mul(config.batch_size, spec.unique_blocks);
    kv = checked_mul(kv, slots); kv = checked_mul(kv, spec.kv_heads);
    kv = checked_mul(kv, head_dim); kv = checked_mul(kv, 2);
    kv = checked_mul(kv, config.activation_bytes);
    const auto activations = static_cast<std::uint64_t>(std::ceil(
        static_cast<long double>(checked_add(state, kv)) * config.activation_multiplier));
    auto total = checked_add(weights, state); total = checked_add(total, kv);
    total = checked_add(total, activations); total = checked_add(total, config.bridge_bytes);
    total = checked_add(total, config.specialist_bytes); total = checked_add(total, config.runtime_bytes);
    total = checked_add(total, config.reserve_bytes);
    const auto headroom = total <= config.budget_bytes
        ? static_cast<std::int64_t>(config.budget_bytes - total)
        : -static_cast<std::int64_t>(total - config.budget_bytes);
    return {parameters, weights, state, kv, activations, config.bridge_bytes,
        config.specialist_bytes, config.runtime_bytes, config.reserve_bytes,
        total, config.budget_bytes, headroom, total <= config.budget_bytes, spec.max_cycles};
}

}  // namespace swegca::world
