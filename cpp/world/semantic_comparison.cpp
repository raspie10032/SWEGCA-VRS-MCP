#include "world/semantic_comparison.hpp"

#include "world/semantic_vrs_ingress.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>

namespace swegca::world {
namespace {

const JsonValue* field(const JsonValue& value, const std::string_view key) {
    if (!value.is_object()) return nullptr;
    const auto found = value.as_object().find(key);
    return found == value.as_object().end() ? nullptr : &found->second;
}

std::string value_type(const JsonValue& value) {
    if (const auto* schema = field(value, "schema")) {
        if (const auto* text = std::get_if<std::string>(&schema->storage())) {
            if (*text == "rozephine-table-value-v1") return "TableValue";
            if (*text == "rozephine-speech-value-v1") return "SpeechValue";
        }
    }
    return std::visit([](const auto& current) -> std::string {
        using T = std::decay_t<decltype(current)>;
        if constexpr (std::is_same_v<T, std::nullptr_t>) return "NoneType";
        if constexpr (std::is_same_v<T, bool>) return "bool";
        if constexpr (std::is_same_v<T, std::int64_t> || std::is_same_v<T, JsonInteger>) return "int";
        if constexpr (std::is_same_v<T, double>) return "float";
        if constexpr (std::is_same_v<T, std::string>) return "str";
        if constexpr (std::is_same_v<T, JsonValue::Array>) return "list";
        return "dict";
    }, value.storage());
}

JsonValue speech_entity(const JsonValue& value) {
    if (std::holds_alternative<std::nullptr_t>(value.storage())) return nullptr;
    return value.at("entity");
}

JsonValue speech_entities(const JsonValue& value) {
    JsonValue::Array result;
    for (const auto& row : value.as_array()) result.push_back(speech_entity(row));
    return result;
}

JsonValue comparison_value(const JsonValue& value) {
    const auto* schema_value = field(value, "schema");
    const auto* schema = schema_value
        ? std::get_if<std::string>(&schema_value->storage()) : nullptr;
    if (!schema) return value;
    if (*schema == "rozephine-speech-value-v1")
        return JsonValue::Array{value.at("kind"), speech_entity(value.at("speaker")),
            speech_entities(value.at("addressees")), speech_entities(value.at("topics")),
            value.at("content")};
    if (*schema == "rozephine-table-value-v1") {
        JsonValue::Array rows;
        for (const auto& row : value.at("rows").as_array()) {
            JsonValue::Array cells;
            for (const auto& cell : row.as_array())
                cells.emplace_back(JsonValue::Array{value_type(cell), cell});
            rows.emplace_back(std::move(cells));
        }
        return JsonValue::Array{value.at("columns"), JsonValue(std::move(rows))};
    }
    return value;
}

std::string key_wire(const SessionSemanticPropositionKey& key) {
    JsonValue::Array qualifiers;
    for (const auto& [kind, value] : key.qualifiers)
        qualifiers.emplace_back(JsonValue::Array{JsonValue(kind), JsonValue(value)});
    return semantic_canonical_json(JsonValue::Array{
        JsonValue(key.subject), JsonValue(key.predicate), JsonValue(key.value_type),
        key.value, JsonValue(std::move(qualifiers)), JsonValue(key.value_kind)});
}

std::string quoted(const std::string& value) {
    return semantic_canonical_json(JsonValue(value));
}

}  // namespace

SessionSemanticPropositionKey prepare_proposition_key(
    const SemanticMeaningUnit& unit) {
    std::set<std::pair<std::string, std::string>> qualifiers;
    for (const auto& qualifier : unit.qualifiers)
        qualifiers.emplace(qualifier.kind, qualifier.value);
    return {unit.subject, unit.predicate, value_type(unit.value),
            comparison_value(unit.value), {qualifiers.begin(), qualifiers.end()},
            unit.value_kind};
}

std::vector<SemanticComparison> compare_semantic_claims(
    const std::vector<SemanticComparisonRow>& rows) {
    std::map<std::string, std::pair<SessionSemanticPropositionKey,
        std::vector<SemanticComparisonRow>>, std::less<>> groups;
    for (const auto& row : rows) {
        if (!row.claim) continue;
        if (!row.event || !row.event->semantic_encoding)
            throw std::invalid_argument(
                "semantic comparison requires cold source-bound handles");
        const auto key = key_wire(row.claim->semantic_key);
        auto [found, inserted] = groups.emplace(
            key, std::pair{row.claim->semantic_key, std::vector<SemanticComparisonRow>{}});
        found->second.second.push_back(row);
    }
    std::vector<SemanticComparison> result;
    for (auto& [wire, group] : groups) {
        (void)wire;
        SemanticComparison comparison;
        comparison.proposition = std::move(group.first);
        std::set<std::pair<std::string, std::string>> sources;
        for (const auto& row : group.second) {
            const auto& polarity = row.claim->semantic_unit.polarity;
            if (polarity == "affirmed") comparison.affirmed.push_back(row);
            else if (polarity == "denied") comparison.denied.push_back(row);
            else comparison.unknown.push_back(row);
            sources.emplace(row.event->semantic_encoding->source_id(),
                            row.event->semantic_encoding->source_revision());
        }
        comparison.parent_sources.assign(sources.begin(), sources.end());
        if (!comparison.affirmed.empty() && !comparison.denied.empty())
            comparison.status = "opposed_interpretations";
        else if (!comparison.affirmed.empty())
            comparison.status = comparison.unknown.empty()
                ? "affirmed_only" : "affirmed_with_unknown";
        else if (!comparison.denied.empty())
            comparison.status = comparison.unknown.empty()
                ? "denied_only" : "denied_with_unknown";
        else comparison.status = "unknown_only";
        result.push_back(std::move(comparison));
    }
    return result;
}

std::string semantic_comparison_text(
    const SemanticComparison& group, const bool include_proposition) {
    const auto& key = group.proposition;
    JsonValue::Array qualifiers;
    for (const auto& [kind, value] : key.qualifiers)
        qualifiers.emplace_back(JsonValue::Array{JsonValue(kind), JsonValue(value)});
    std::string heading;
    if (include_proposition) {
        heading = "해석 비교: 대상 " + quoted(key.subject) + ", 관계 " +
            quoted(key.predicate) + ", 값 " + semantic_canonical_json(key.value) +
            ", 한정 조건 " + semantic_canonical_json(JsonValue(qualifiers)) + ". ";
    } else {
        JsonValue::Array references;
        for (const auto* rows : {&group.affirmed, &group.denied, &group.unknown})
            for (const auto& row : *rows)
                references.emplace_back(JsonValue::Array{
                    JsonValue(row.claim->address.episode_id),
                    JsonValue(static_cast<std::int64_t>(row.claim->address.step)),
                    JsonValue(static_cast<std::int64_t>(row.claim->address.claim))});
        heading = "미채택 발화 해석 비교: 주장 주소 " +
            semantic_canonical_json(JsonValue(std::move(references))) +
            ", 한정 조건 " + semantic_canonical_json(JsonValue(qualifiers)) +
            ". 발화 본문과 역할 제안은 main의 semantic_comparisons 감사 참조에 보존한다. ";
    }
    std::string decision;
    if (group.status == "opposed_interpretations")
        decision = "같은 명제에 대한 해석 제안끼리 반대이므로 어느 해석이 맞는지 확정하지 않는다. ";
    else if (group.status == "unknown_only")
        decision = "해석 제안은 이 명제에 대해 미확정이다. ";
    else {
        decision = "현재 비교한 제안 중 확정 방향을 적은 것은 " +
            std::string(group.affirmed.empty() ? "부정" : "긍정") + "이며";
        decision += group.unknown.empty()
            ? " 반대 방향 제안은 없다. " : " 미확정 제안도 남아 있다. ";
    }
    return heading + decision +
        "제안의 일치·대립은 실제 사실 확인이나 현재 증거 충돌 판정이 아니며, "
        "독립 증거를 추가하거나 기존 경험 승격을 철회하지 않는다.";
}

}  // namespace swegca::world
