#include "world/definition_contract.hpp"

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>

namespace swegca::world {
namespace {

struct Decoded final {
    char32_t value;
    std::size_t next;
};

Decoded decode_utf8(const std::string_view text, const std::size_t at) {
    const auto first = static_cast<std::uint8_t>(text[at]);
    if (first < 0x80U) return {first, at + 1};
    std::size_t count = 0;
    char32_t value = 0;
    char32_t minimum = 0;
    if ((first & 0xe0U) == 0xc0U) {
        count = 2; value = first & 0x1fU; minimum = 0x80;
    } else if ((first & 0xf0U) == 0xe0U) {
        count = 3; value = first & 0x0fU; minimum = 0x800;
    } else if ((first & 0xf8U) == 0xf0U) {
        count = 4; value = first & 0x07U; minimum = 0x10000;
    } else {
        throw std::invalid_argument("definition text is not valid UTF-8");
    }
    if (count > text.size() - at) {
        throw std::invalid_argument("definition text is not valid UTF-8");
    }
    for (std::size_t offset = 1; offset != count; ++offset) {
        const auto byte = static_cast<std::uint8_t>(text[at + offset]);
        if ((byte & 0xc0U) != 0x80U) {
            throw std::invalid_argument("definition text is not valid UTF-8");
        }
        value = (value << 6U) | (byte & 0x3fU);
    }
    if (value < minimum || value > 0x10ffffU ||
        (value >= 0xd800U && value <= 0xdfffU)) {
        throw std::invalid_argument("definition text is not valid UTF-8");
    }
    return {value, at + count};
}

bool python_whitespace(const char32_t value) noexcept {
    if ((value >= 0x09 && value <= 0x0d) || (value >= 0x1c && value <= 0x20)) {
        return true;
    }
    constexpr std::array<char32_t, 12> remaining{
        0x85, 0xa0, 0x1680, 0x2007, 0x2028, 0x2029,
        0x202f, 0x205f, 0x3000, 0x2000, 0x2001, 0x2002};
    if (value >= 0x2000 && value <= 0x200a) return true;
    for (const auto item : remaining) {
        if (value == item) return true;
    }
    return false;
}

std::string python_strip(const std::string_view value) {
    std::vector<std::pair<std::size_t, Decoded>> points;
    for (std::size_t at = 0; at != value.size();) {
        const auto decoded = decode_utf8(value, at);
        points.push_back({at, decoded});
        at = decoded.next;
    }
    std::size_t first = 0;
    while (first != points.size() && python_whitespace(points[first].second.value)) ++first;
    std::size_t last = points.size();
    while (last != first && python_whitespace(points[last - 1].second.value)) --last;
    if (first == last) return {};
    const auto begin = points[first].first;
    const auto end = points[last - 1].second.next;
    return std::string(value.substr(begin, end - begin));
}

void require_scalar(const std::string& value, const char* name) {
    if (python_strip(value).empty()) {
        throw std::invalid_argument(
            std::string("definition_contract.") + name + " must be a non-empty string");
    }
}

std::vector<std::string> normalized_sequence(
    const std::vector<std::string>& values, const char* name,
    const bool require_nonempty) {
    std::vector<std::string> normalized;
    normalized.reserve(values.size());
    std::unordered_set<std::string> unique;
    for (const auto& value : values) {
        auto stripped = python_strip(value);
        if (stripped.empty()) {
            throw std::invalid_argument(std::string("definition_contract.") + name +
                                        " must not be empty");
        }
        if (!unique.insert(stripped).second) {
            throw std::invalid_argument(std::string("definition_contract.") + name +
                                        " must not contain duplicates");
        }
        normalized.push_back(std::move(stripped));
    }
    if (require_nonempty && normalized.empty()) {
        throw std::invalid_argument(std::string("definition_contract.") + name +
                                    " must not be empty");
    }
    return normalized;
}

}  // namespace

DefinitionAssessment validate_definition_contract(
    const DefinitionContract& contract, const DefinitionInvariants& invariants) {
    require_scalar(contract.phenomenon, "phenomenon");
    require_scalar(contract.observation_unit, "observation_unit");
    require_scalar(contract.identity_rule, "identity_rule");
    require_scalar(contract.operator_definition, "operator_definition");
    require_scalar(contract.evidence_address, "evidence_address");
    require_scalar(contract.unknown_policy, "unknown_policy");
    require_scalar(contract.transition_rule, "transition_rule");

    const auto state_variables = normalized_sequence(
        contract.state_variables, "state_variables", true);
    const auto counterfactuals = normalized_sequence(
        contract.counterfactuals, "counterfactuals", true);
    auto decision_labels = normalized_sequence(
        contract.decision_labels, "decision_labels", true);
    auto unresolved = normalized_sequence(
        contract.unresolved_definitions, "unresolved_definitions", false);
    const bool writes_enabled = invariants.world_write_enabled ||
        invariants.episodic_memory_write_enabled ||
        invariants.semantic_memory_write_enabled;
    if (!unresolved.empty() && writes_enabled) {
        throw std::invalid_argument(
            "unresolved definitions prohibit World and memory writes");
    }
    return {
        unresolved.empty() ? DefinitionStatus::complete : DefinitionStatus::partial,
        std::move(unresolved), state_variables.size(), counterfactuals.size(),
        std::move(decision_labels), writes_enabled};
}

}  // namespace swegca::world
