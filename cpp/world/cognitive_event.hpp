#pragma once

#include "world/cognitive_state.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace swegca::world {

enum class EvidenceKind : std::uint8_t {
    learned_prediction,
    observed_evidence,
    deterministic_simulation,
    user_claim,
    external_model_claim,
};

[[nodiscard]] std::string_view evidence_kind_name(EvidenceKind kind) noexcept;
[[nodiscard]] EvidenceKind evidence_kind_from_name(std::string_view name);

struct EventSource final {
    std::string representation;
    std::string adapter;
    std::string source_ref;

    EventSource(std::string representation, std::string adapter, std::string source_ref);
    [[nodiscard]] JsonValue to_dict() const;
    [[nodiscard]] static EventSource from_dict(const JsonValue& payload);
    friend bool operator==(const EventSource&, const EventSource&) = default;
};

struct EvidenceClaim final {
    std::string predicate;
    JsonValue value;
    double confidence{};
    std::optional<std::string> subject;

    EvidenceClaim(std::string predicate, JsonValue value, double confidence,
                  std::optional<std::string> subject = std::nullopt);
    [[nodiscard]] JsonValue to_dict() const;
    [[nodiscard]] static EvidenceClaim from_dict(const JsonValue& payload);
    friend bool operator==(const EvidenceClaim&, const EvidenceClaim&) = default;
};

class CognitiveEvent final {
public:
    CognitiveEvent(std::string event_id, std::string event_type, EventSource source,
                   std::vector<EvidenceClaim> claims, std::vector<std::string> evidence_refs,
                   EvidenceKind evidence_kind, JsonValue::Object metadata = {});

    [[nodiscard]] std::string_view event_id() const noexcept;
    [[nodiscard]] std::string_view event_type() const noexcept;
    [[nodiscard]] const EventSource& source() const noexcept;
    [[nodiscard]] std::span<const EvidenceClaim> claims() const noexcept;
    [[nodiscard]] std::span<const std::string> evidence_refs() const noexcept;
    [[nodiscard]] EvidenceKind evidence_kind() const noexcept;
    [[nodiscard]] const JsonValue::Object& metadata() const noexcept;
    [[nodiscard]] JsonValue to_dict() const;
    [[nodiscard]] static CognitiveEvent from_dict(const JsonValue& payload);
    friend bool operator==(const CognitiveEvent&, const CognitiveEvent&) = default;

private:
    std::string event_id_;
    std::string event_type_;
    EventSource source_;
    std::vector<EvidenceClaim> claims_;
    std::vector<std::string> evidence_refs_;
    EvidenceKind evidence_kind_;
    JsonValue::Object metadata_;
};

}  // namespace swegca::world
