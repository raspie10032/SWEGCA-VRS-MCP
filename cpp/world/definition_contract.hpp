#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace swegca::world {

// Typed form of the pinned Python definition_contract mapping. Empty fields
// remain representable so validation can reject incomplete external values.
struct DefinitionContract final {
    std::string phenomenon;
    std::vector<std::string> state_variables;
    std::string observation_unit;
    std::string identity_rule;
    std::string operator_definition;
    std::string evidence_address;
    std::string unknown_policy;
    std::string transition_rule;
    std::vector<std::string> counterfactuals;
    std::vector<std::string> decision_labels;
    std::vector<std::string> unresolved_definitions;
};

struct DefinitionInvariants final {
    bool world_write_enabled = false;
    bool episodic_memory_write_enabled = false;
    bool semantic_memory_write_enabled = false;
};

enum class DefinitionStatus { partial, complete };

struct DefinitionAssessment final {
    DefinitionStatus status = DefinitionStatus::partial;
    std::vector<std::string> unresolved_definitions;
    std::size_t state_variable_count = 0;
    std::size_t counterfactual_count = 0;
    std::vector<std::string> decision_labels;
    bool writes_enabled = false;
};

[[nodiscard]] DefinitionAssessment validate_definition_contract(
    const DefinitionContract& contract,
    const DefinitionInvariants& invariants = {});

[[nodiscard]] constexpr std::string_view definition_contract_source_sha256() noexcept {
    return "56b7269440b762674e4667d202fed247b6384beb74bcd254d2fc24c4e2627551";
}

}  // namespace swegca::world
