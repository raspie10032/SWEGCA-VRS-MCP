#include "world/definition_contract.hpp"

#include <cassert>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace swegca::world;

namespace {

DefinitionContract contract(std::vector<std::string> unresolved = {}) {
    return {
        "an observable state transition",
        {"identity", "visibility", "attribute"},
        "pre/contact/post evidence",
        "one object instance across phases",
        "an action that changes one named attribute",
        "source plus timestamp plus region",
        "unobserved values remain unknown",
        "before plus action and evidence yields after",
        {"missing phase", "wrong object"},
        {"support", "refute", "insufficient"},
        std::move(unresolved)};
}

template<class Function> void fails(Function&& function) {
    bool failed = false;
    try { function(); } catch (const std::invalid_argument&) { failed = true; }
    assert(failed);
}

}  // namespace

int main() {
    {
        const auto assessment = validate_definition_contract(contract());
        assert(assessment.status == DefinitionStatus::complete);
        assert(assessment.state_variable_count == 3);
        assert(assessment.counterfactual_count == 2);
        assert(!assessment.writes_enabled);
    }
    {
        const auto assessment = validate_definition_contract(
            contract({" occluded object state "}));
        assert(assessment.status == DefinitionStatus::partial);
        assert(assessment.unresolved_definitions ==
               std::vector<std::string>{"occluded object state"});
    }
    fails([] {
        (void)validate_definition_contract(
            contract({"identity across occlusion"}),
            DefinitionInvariants{true, false, false});
    });
    fails([] {
        auto value = contract();
        value.identity_rule.clear();
        (void)validate_definition_contract(value);
    });
    fails([] {
        auto value = contract();
        value.identity_rule = "\xe3\x80\x80";
        (void)validate_definition_contract(value);
    });
    fails([] {
        auto value = contract();
        value.counterfactuals = {"same", " same "};
        (void)validate_definition_contract(value);
    });
    fails([] {
        auto value = contract();
        value.unresolved_definitions = {std::string("\xc0\x80", 2)};
        (void)validate_definition_contract(value);
    });
    assert(definition_contract_source_sha256() ==
           "56b7269440b762674e4667d202fed247b6384beb74bcd254d2fc24c4e2627551");
    std::cout << "definition contract tests passed\n";
}
