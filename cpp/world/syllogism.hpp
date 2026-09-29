#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace swegca::world {

inline constexpr std::string_view syllogism_source_sha256 =
    "394e8fa32bebd6f11f2a1803d0ff5aaba890ab9a9ee5328877843f4ed55db2a5";

struct CategoricalStatement final {
    char quantifier{};
    std::string subject;
    std::string predicate;

    CategoricalStatement(char quantifier, std::string subject,
                         std::string predicate);
};

struct SyllogismForm final {
    bool valid{};
    std::string reason;
    std::optional<std::uint8_t> figure;
    std::optional<std::string> mood;
    std::optional<std::uint8_t> countermodel;
    std::string semantics{"categorical_sets_without_existential_import"};
    bool grants_authority{};
    bool establishes_premise_truth{};
};

// Non-authoritative categorical-form check. The caller owns premise identity,
// current-snapshot binding, uncertainty, outcomes and conflicting evidence.
[[nodiscard]] SyllogismForm check_syllogism(
    const CategoricalStatement& major,
    const CategoricalStatement& minor,
    const CategoricalStatement& conclusion);

}  // namespace swegca::world
