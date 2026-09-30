#include "world/syllogism_sources.hpp"

#include <array>
#include <set>
#include <stdexcept>
#include <utility>

namespace swegca::world {
namespace {

const JsonValue* find(const JsonValue::Object& object, const std::string_view key) {
    const auto item = object.find(key);
    return item == object.end() ? nullptr : &item->second;
}

const std::string& string_field(const JsonValue::Object& object,
                                const std::string_view key,
                                const char* reason) {
    const auto* value = find(object, key);
    const auto* text = value ? std::get_if<std::string>(&value->storage()) : nullptr;
    if (!text) throw std::invalid_argument(reason);
    return *text;
}

bool schema_is(const JsonValue::Object& object) {
    const auto* value = find(object, "schema_version");
    const auto* text = value ? std::get_if<std::string>(&value->storage()) : nullptr;
    return text && *text == categorical_premises_schema;
}

}  // namespace

PremiseReadView::PremiseReadView(
    std::string memory_snapshot_id_value,
    std::shared_ptr<const AddressMap> by_address,
    std::shared_ptr<const PairMap> by_pair,
    std::shared_ptr<const TermMap> by_term)
    : memory_snapshot_id(std::move(memory_snapshot_id_value)),
      by_address_(std::move(by_address)), by_pair_(std::move(by_pair)),
      by_term_(std::move(by_term)) {}

PremiseReadView PremiseReadView::from_indexes(
    std::string memory_snapshot_id,
    std::shared_ptr<const AddressMap> by_address,
    std::shared_ptr<const PairMap> by_pair,
    std::shared_ptr<const TermMap> by_term) {
    if (!by_address || !by_pair || !by_term)
        throw std::invalid_argument("premise index required");
    return PremiseReadView(std::move(memory_snapshot_id), std::move(by_address),
                           std::move(by_pair), std::move(by_term));
}

const SourcePremise& PremiseReadView::premise(
    const PremiseAddress& address) const {
    const auto item = by_address_->find(address);
    if (item == by_address_->end()) throw std::out_of_range("premise address unavailable");
    return item->second;
}

std::vector<PremiseAddress> PremiseReadView::term_addresses(
    const std::string_view term) const {
    const auto item = by_term_->find(term);
    return item == by_term_->end() ? std::vector<PremiseAddress>{} : item->second;
}

std::vector<std::optional<OpposedPremiseClaims>>
PremiseReadView::opposition_steps(const PremiseAddress& address) const {
    const auto& source = premise(address);
    const auto& statement = source.statement;
    const char opposite = statement.quantifier == 'A' ? 'O' :
        statement.quantifier == 'O' ? 'A' :
        statement.quantifier == 'E' ? 'I' : 'E';
    std::vector<Pair> pairs{{statement.subject, statement.predicate}};
    if ((statement.quantifier == 'E' || statement.quantifier == 'I') &&
        statement.subject != statement.predicate)
        pairs.emplace_back(statement.predicate, statement.subject);
    std::vector<std::optional<OpposedPremiseClaims>> result;
    for (const auto& pair : pairs) {
        const auto rows = by_pair_->find(pair);
        if (rows == by_pair_->end()) continue;
        for (const auto& candidate : rows->second) {
            const auto& opposed = premise(candidate);
            if (opposed.statement.quantifier == opposite)
                result.emplace_back(OpposedPremiseClaims{source, opposed});
            else
                result.emplace_back(std::nullopt);
        }
    }
    return result;
}

std::vector<SourceSyllogism> PremiseReadView::attempts(
    const PremiseAddress& major_address,
    const CategoricalStatement& conclusion) const {
    const auto& major = premise(major_address);
    const std::set<std::string> terms{
        major.statement.subject, major.statement.predicate};
    const auto& subject = conclusion.subject;
    const auto& predicate = conclusion.predicate;
    if (terms.size() != 2 || !terms.contains(predicate) || terms.contains(subject))
        throw std::invalid_argument("major_conclusion_middle_term_mismatch");
    std::string middle;
    for (const auto& term : terms)
        if (term != predicate) middle = term;
    const std::array<Pair, 2> pairs{{{subject, middle}, {middle, subject}}};
    std::vector<SourceSyllogism> result;
    for (const auto& pair : pairs) {
        const auto rows = by_pair_->find(pair);
        if (rows == by_pair_->end()) continue;
        for (const auto& address : rows->second) {
            const auto& minor = premise(address);
            result.push_back({memory_snapshot_id, major, minor, conclusion,
                              check_syllogism(major.statement, minor.statement,
                                              conclusion)});
        }
    }
    return result;
}

PremiseSegment::PremiseSegment(
    std::shared_ptr<const HotMemoryIndex> memory,
    std::shared_ptr<const PremiseReadView::AddressMap> by_address,
    std::shared_ptr<const PremiseReadView::PairMap> by_pair,
    std::shared_ptr<const PremiseReadView::TermMap> by_term,
    std::vector<std::string> inspected,
    std::vector<std::string> absent)
    : memory_snapshot_id(memory ? std::string(memory->snapshot_id()) : std::string{}),
      inspected_episode_ids(std::move(inspected)),
      episodes_without_explicit_premises(std::move(absent)),
      memory_(std::move(memory)), by_address_(std::move(by_address)),
      by_pair_(std::move(by_pair)), by_term_(std::move(by_term)) {}

PremiseReadView PremiseSegment::bind(
    const std::shared_ptr<const HotMemoryIndex>& memory) const {
    if (memory.get() != memory_.get() || !memory ||
        memory->snapshot_id() != memory_snapshot_id)
        throw std::invalid_argument(
            "premise source generation changed; bind a prepared successor");
    return PremiseReadView(memory_snapshot_id, by_address_, by_pair_, by_term_);
}

PremiseSegment prepare_premise_segment(
    std::shared_ptr<const HotMemoryIndex> memory,
    const std::vector<std::string>& episode_ids,
    const std::function<void(const MemoryEpisode&)>& admitted_episode) {
    if (!memory || memory->lookup_requires_io())
        throw std::invalid_argument("prepared hot memory required for premise admission");
    const std::string snapshot(memory->snapshot_id());
    auto by_address = std::make_shared<PremiseReadView::AddressMap>();
    auto by_pair = std::make_shared<PremiseReadView::PairMap>();
    auto by_term = std::make_shared<PremiseReadView::TermMap>();
    std::vector<std::string> inspected;
    std::vector<std::string> absent;
    std::set<std::string, std::less<>> seen;
    for (const auto& identifier : episode_ids) {
        if (identifier.empty())
            throw std::invalid_argument("main admission episode ID required");
        if (!seen.insert(identifier).second)
            throw std::invalid_argument("duplicate admission episode ID");
        const auto& episode = memory->episode(identifier);
        if (episode.episode_id != identifier)
            throw std::invalid_argument("source address routing mismatch");
        if (admitted_episode) admitted_episode(episode);
        inspected.push_back(identifier);
        bool found{};
        for (std::size_t step_index = 0; step_index < episode.steps.size(); ++step_index) {
            const auto& step = episode.steps[step_index];
            const JsonValue::Object* observation = &step.observation;
            if (const auto* nested = find(step.observation, "categorical_premises")) {
                if (schema_is(step.observation))
                    throw std::invalid_argument(
                        "ambiguous top-level and nested categorical premises");
                if (!nested->is_object())
                    throw std::invalid_argument(
                        "explicit categorical premise subrecord required");
                observation = &nested->as_object();
            }
            if (!schema_is(*observation)) continue;
            const auto* statements = find(*observation, "statements");
            if (!statements || !statements->is_array())
                throw std::invalid_argument(
                    "explicit immutable categorical statement sequence required");
            for (std::size_t position = 0;
                 position < statements->as_array().size(); ++position) {
                const auto& value = statements->as_array()[position];
                if (!value.is_object() || value.as_object().size() != 3 ||
                    !value.as_object().contains("quantifier") ||
                    !value.as_object().contains("subject") ||
                    !value.as_object().contains("predicate"))
                    throw std::invalid_argument(
                        "exact categorical statement fields required");
                const auto& row = value.as_object();
                const auto& quantifier = string_field(
                    row, "quantifier", "exact categorical statement fields required");
                if (quantifier.size() != 1)
                    throw std::invalid_argument(
                        "explicit A/E/I/O quantifier required; no co-occurrence inference");
                CategoricalStatement statement(
                    quantifier.front(),
                    string_field(row, "subject", "exact categorical statement fields required"),
                    string_field(row, "predicate", "exact categorical statement fields required"));
                PremiseAddress address{identifier, step_index, position};
                SourcePremise premise{
                    address, statement, episode.revision, episode.source_addresses,
                    step.evidence_refs, step.outcome, episode.verification_state};
                by_address->emplace(address, premise);
                (*by_pair)[{statement.subject, statement.predicate}].push_back(address);
                (*by_term)[statement.subject].push_back(address);
                if (statement.predicate != statement.subject)
                    (*by_term)[statement.predicate].push_back(address);
                found = true;
            }
        }
        if (!found) absent.push_back(identifier);
    }
    if (memory->snapshot_id() != snapshot)
        throw std::invalid_argument(
            "source generation changed during premise preparation");
    return PremiseSegment(std::move(memory), std::move(by_address),
                          std::move(by_pair), std::move(by_term),
                          std::move(inspected), std::move(absent));
}

}  // namespace swegca::world
