#include "world/syllogism.hpp"

#include <array>
#include <bitset>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace swegca::world {
namespace {

using Pair = std::pair<unsigned, unsigned>;
using Figure = std::pair<Pair, Pair>;

constexpr std::array<Figure, 4> figures{{
    {{{2, 1}, {0, 2}}},
    {{{1, 2}, {0, 2}}},
    {{{2, 1}, {2, 0}}},
    {{{1, 2}, {2, 0}}},
}};

constexpr std::array<char, 4> quantifiers{'A', 'E', 'I', 'O'};

bool holds(const std::uint8_t world, const char quantifier,
           const unsigned subject, const unsigned predicate) {
    std::uint8_t wanted{};
    for (unsigned region = 0; region < 8; ++region) {
        if ((region & (1U << subject)) != 0 &&
            (((region & (1U << predicate)) != 0) ==
             (quantifier == 'E' || quantifier == 'I'))) {
            wanted = static_cast<std::uint8_t>(wanted | (1U << region));
        }
    }
    const bool exists = (world & wanted) != 0;
    return quantifier == 'A' || quantifier == 'E' ? !exists : exists;
}

std::size_t mood_index(const char a, const char b, const char c) {
    const auto index = [](const char value) -> std::size_t {
        for (std::size_t i = 0; i < quantifiers.size(); ++i)
            if (quantifiers[i] == value) return i;
        throw std::invalid_argument("explicit A/E/I/O quantifier required; no co-occurrence inference");
    };
    return index(a) * 16 + index(b) * 4 + index(c);
}

using FormTable = std::array<std::array<SyllogismForm, 64>, 4>;

FormTable build_forms() {
    FormTable forms{};
    for (std::size_t fi = 0; fi < figures.size(); ++fi) {
        const auto [major, minor] = figures[fi];
        for (const char a : quantifiers) {
            for (const char b : quantifiers) {
                for (const char c : quantifiers) {
                    bool any_premises{};
                    std::optional<std::uint8_t> countermodel;
                    for (unsigned world = 0; world < 256; ++world) {
                        const auto state = static_cast<std::uint8_t>(world);
                        if (!holds(state, a, major.first, major.second) ||
                            !holds(state, b, minor.first, minor.second)) continue;
                        any_premises = true;
                        if (!countermodel && !holds(state, c, 0, 1))
                            countermodel = state;
                    }
                    const std::string mood{a, b, c};
                    auto& result = forms[fi][mood_index(a, b, c)];
                    result.figure = static_cast<std::uint8_t>(fi + 1);
                    result.mood = mood;
                    if (!any_premises) {
                        result.reason = "inconsistent_premises";
                    } else if (countermodel) {
                        result.reason = "conclusion_not_entailed";
                        result.countermodel = countermodel;
                    } else {
                        result.valid = true;
                        result.reason = "categorical_form_entailed";
                    }
                }
            }
        }
    }
    return forms;
}

const FormTable forms = build_forms();

}  // namespace

CategoricalStatement::CategoricalStatement(
    const char quantifier_value, std::string subject_value,
    std::string predicate_value)
    : quantifier(quantifier_value), subject(std::move(subject_value)),
      predicate(std::move(predicate_value)) {
    if (quantifier != 'A' && quantifier != 'E' && quantifier != 'I' &&
        quantifier != 'O')
        throw std::invalid_argument(
            "explicit A/E/I/O quantifier required; no co-occurrence inference");
    if (subject.empty() || predicate.empty())
        throw std::invalid_argument("nonempty explicit term identities required");
}

SyllogismForm check_syllogism(
    const CategoricalStatement& major,
    const CategoricalStatement& minor,
    const CategoricalStatement& conclusion) {
    const auto& s = conclusion.subject;
    const auto& p = conclusion.predicate;
    const std::set<std::string> terms{
        major.subject, major.predicate, minor.subject, minor.predicate, s, p};
    if (terms.size() != 3 || s == p)
        return {false, "three_distinct_terms_required"};

    const auto identity = [&](const std::string& term) -> unsigned {
        if (term == s) return 0;
        if (term == p) return 1;
        return 2;
    };
    const Figure shape{{identity(major.subject), identity(major.predicate)},
                       {identity(minor.subject), identity(minor.predicate)}};
    for (std::size_t fi = 0; fi < figures.size(); ++fi) {
        if (shape == figures[fi])
            return forms[fi][mood_index(major.quantifier, minor.quantifier,
                                        conclusion.quantifier)];
    }
    return {false, "major_minor_middle_term_mismatch"};
}

}  // namespace swegca::world
