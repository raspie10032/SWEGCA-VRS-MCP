#include "world/cognitive_event.hpp"

#include <algorithm>
#include <cmath>
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

void reject_unknown_keys(const JsonValue::Object& object,
                         const std::span<const std::string_view> allowed) {
    for (const auto& [key, value] : object) {
        static_cast<void>(value);
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) {
            throw std::invalid_argument("unexpected object field: " + key);
        }
    }
}

[[nodiscard]] const JsonValue* optional(const JsonValue::Object& object,
                                        const std::string_view key) noexcept {
    const auto found = object.find(key);
    return found == object.end() ? nullptr : &found->second;
}

[[nodiscard]] JsonValue string_array(const std::span<const std::string> values) {
    JsonValue::Array result;
    result.reserve(values.size());
    for (const auto& value : values) {
        result.emplace_back(value);
    }
    return result;
}

[[nodiscard]] std::vector<std::string> strings(const JsonValue& value) {
    std::vector<std::string> result;
    result.reserve(value.as_array().size());
    for (const auto& item : value.as_array()) {
        result.emplace_back(item.as_string());
    }
    return result;
}

}  // namespace

std::string_view evidence_kind_name(const EvidenceKind kind) noexcept {
    switch (kind) {
    case EvidenceKind::learned_prediction:
        return "learned_prediction";
    case EvidenceKind::observed_evidence:
        return "observed_evidence";
    case EvidenceKind::deterministic_simulation:
        return "deterministic_simulation";
    case EvidenceKind::user_claim:
        return "user_claim";
    case EvidenceKind::external_model_claim:
        return "external_model_claim";
    }
    return {};
}

EvidenceKind evidence_kind_from_name(const std::string_view name) {
    if (name == "learned_prediction") {
        return EvidenceKind::learned_prediction;
    }
    if (name == "observed_evidence") {
        return EvidenceKind::observed_evidence;
    }
    if (name == "deterministic_simulation") {
        return EvidenceKind::deterministic_simulation;
    }
    if (name == "user_claim") {
        return EvidenceKind::user_claim;
    }
    if (name == "external_model_claim") {
        return EvidenceKind::external_model_claim;
    }
    throw std::invalid_argument("unsupported evidence kind: " + std::string(name));
}

EventSource::EventSource(std::string representation_value, std::string adapter_value,
                         std::string source_ref_value)
    : representation(std::move(representation_value)), adapter(std::move(adapter_value)),
      source_ref(std::move(source_ref_value)) {
    require_text(representation, "representation");
    require_text(adapter, "adapter");
    require_text(source_ref, "source_ref");
}

JsonValue EventSource::to_dict() const {
    return JsonValue::Object{{"representation", representation},
                             {"adapter", adapter},
                             {"source_ref", source_ref}};
}

EventSource EventSource::from_dict(const JsonValue& payload) {
    const auto& object = payload.as_object();
    constexpr std::string_view allowed[]{"representation", "adapter", "source_ref"};
    reject_unknown_keys(object, allowed);
    return EventSource(std::string(payload.at("representation").as_string()),
                       std::string(payload.at("adapter").as_string()),
                       std::string(payload.at("source_ref").as_string()));
}

EvidenceClaim::EvidenceClaim(std::string predicate_value, JsonValue claim_value,
                             const double confidence_value,
                             std::optional<std::string> subject_value)
    : predicate(std::move(predicate_value)), value(std::move(claim_value)),
      confidence(confidence_value), subject(std::move(subject_value)) {
    require_text(predicate, "predicate");
    if (subject) {
        require_text(*subject, "subject");
    }
    if (!std::isfinite(confidence) || confidence < 0.0 || confidence > 1.0) {
        throw std::invalid_argument("confidence must be finite and within [0, 1]");
    }
}

JsonValue EvidenceClaim::to_dict() const {
    return JsonValue::Object{{"predicate", predicate},
                             {"value", value},
                             {"confidence", confidence},
                             {"subject", subject ? JsonValue(*subject) : JsonValue(nullptr)}};
}

EvidenceClaim EvidenceClaim::from_dict(const JsonValue& payload) {
    const auto& object = payload.as_object();
    constexpr std::string_view allowed[]{"predicate", "value", "confidence", "subject"};
    reject_unknown_keys(object, allowed);
    std::optional<std::string> subject;
    if (const auto* value = optional(object, "subject");
        value && !std::holds_alternative<std::nullptr_t>(value->storage())) {
        subject = std::string(value->as_string());
    }
    return EvidenceClaim(std::string(payload.at("predicate").as_string()), payload.at("value"),
                         payload.at("confidence").as_number(), std::move(subject));
}

CognitiveEvent::CognitiveEvent(std::string id, std::string type, EventSource source_value,
                               std::vector<EvidenceClaim> claims_value,
                               std::vector<std::string> references, const EvidenceKind kind,
                               JsonValue::Object metadata_value)
    : event_id_(std::move(id)), event_type_(std::move(type)), source_(std::move(source_value)),
      claims_(std::move(claims_value)), evidence_refs_(std::move(references)),
      evidence_kind_(kind), metadata_(std::move(metadata_value)) {
    require_text(event_id_, "event_id");
    require_text(event_type_, "event_type");
    if (evidence_refs_.empty()) {
        throw std::invalid_argument(
            "evidence_refs must preserve at least one provenance address");
    }
    for (const auto& reference : evidence_refs_) {
        if (!has_text(reference)) {
            throw std::invalid_argument("evidence_refs must not contain empty addresses");
        }
    }
    if (evidence_kind_name(evidence_kind_).empty()) {
        throw std::invalid_argument("unsupported evidence kind");
    }
}

std::string_view CognitiveEvent::event_id() const noexcept { return event_id_; }
std::string_view CognitiveEvent::event_type() const noexcept { return event_type_; }
const EventSource& CognitiveEvent::source() const noexcept { return source_; }
std::span<const EvidenceClaim> CognitiveEvent::claims() const noexcept { return claims_; }
std::span<const std::string> CognitiveEvent::evidence_refs() const noexcept {
    return evidence_refs_;
}
EvidenceKind CognitiveEvent::evidence_kind() const noexcept { return evidence_kind_; }
const JsonValue::Object& CognitiveEvent::metadata() const noexcept { return metadata_; }

JsonValue CognitiveEvent::to_dict() const {
    JsonValue::Array claims;
    claims.reserve(claims_.size());
    for (const auto& claim : claims_) {
        claims.push_back(claim.to_dict());
    }
    return JsonValue::Object{{"event_id", event_id_},
                             {"event_type", event_type_},
                             {"source", source_.to_dict()},
                             {"claims", std::move(claims)},
                             {"evidence_refs", string_array(evidence_refs_)},
                             {"evidence_kind", evidence_kind_name(evidence_kind_)},
                             {"metadata", metadata_}};
}

CognitiveEvent CognitiveEvent::from_dict(const JsonValue& payload) {
    const auto& object = payload.as_object();
    std::vector<EvidenceClaim> claims;
    if (const auto* values = optional(object, "claims")) {
        claims.reserve(values->as_array().size());
        for (const auto& claim : values->as_array()) {
            claims.push_back(EvidenceClaim::from_dict(claim));
        }
    }
    return CognitiveEvent(
        std::string(payload.at("event_id").as_string()),
        std::string(payload.at("event_type").as_string()),
        EventSource::from_dict(payload.at("source")), std::move(claims),
        strings(payload.at("evidence_refs")),
        evidence_kind_from_name(payload.at("evidence_kind").as_string()),
        optional(object, "metadata") ? optional(object, "metadata")->as_object()
                                     : JsonValue::Object{});
}

}  // namespace swegca::world
