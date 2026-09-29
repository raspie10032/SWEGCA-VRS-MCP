#include "world/offline_semantic_batch.hpp"

#include "world/provider_cancellation.hpp"
#include "world/semantic_response_error.hpp"

#include <algorithm>
#include <chrono>
#include <deque>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

namespace swegca::world {
namespace {

std::int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

const JsonValue* find(const JsonValue::Object& object, const std::string_view key) {
    const auto item = object.find(key);
    return item == object.end() ? nullptr : &item->second;
}

const std::string& text(const JsonValue::Object& object, const std::string_view key) {
    const auto* value = find(object, key);
    const auto* result = value ? std::get_if<std::string>(&value->storage()) : nullptr;
    if (!result) throw std::invalid_argument("semantic_failure_receipt_changed");
    return *result;
}

std::int64_t integer(const JsonValue::Object& object, const std::string_view key) {
    const auto* value = find(object, key);
    const auto* result = value ? std::get_if<std::int64_t>(&value->storage()) : nullptr;
    if (!result) throw std::invalid_argument("semantic_failure_receipt_changed");
    return *result;
}

bool boolean(const JsonValue::Object& object, const std::string_view key) {
    const auto* value = find(object, key);
    const auto* result = value ? std::get_if<bool>(&value->storage()) : nullptr;
    if (!result) throw std::invalid_argument("semantic_failure_receipt_changed");
    return *result;
}

std::vector<std::string> unit_references(const SemanticMeaningUnit& unit) {
    std::vector<std::string> result = unit.anchors;
    for (const auto& qualifier : unit.qualifiers)
        for (const auto& anchor : qualifier.anchors)
            if (std::ranges::find(result, anchor) == result.end()) result.push_back(anchor);
    return result;
}

using Eligible = std::map<std::string, std::set<std::size_t>, std::less<>>;

Eligible validate_retained_units(
    const std::vector<SemanticEncoding>& proposals,
    const std::vector<SemanticPreparationFailure>& failures) {
    std::map<std::string, const SemanticEncoding*, std::less<>> by_source;
    Eligible eligible;
    for (const auto& proposal : proposals) {
        by_source.emplace(proposal.source_id, &proposal);
        eligible.emplace(proposal.source_id, std::set<std::size_t>{});
    }
    std::map<std::string,
             std::map<std::string, std::deque<std::size_t>, std::less<>>, std::less<>> remaining;
    for (const auto& failure : failures) {
        if (!failure.partial_unit_count) continue;
        const auto found = by_source.find(failure.source_id);
        if (found == by_source.end() ||
            found->second->source_revision != failure.source_revision ||
            !failure.response_utf8)
            throw std::invalid_argument("partial_semantic_source_changed");
        const auto& proposal = *found->second;
        const auto units = complete_semantic_unit_prefix(
            *failure.response_utf8, proposal.anchors);
        if (units.size() != failure.partial_unit_count)
            throw std::invalid_argument("partial_semantic_units_changed");
        auto& positions = remaining[proposal.source_id];
        if (positions.empty()) {
            for (std::size_t index = 0; index < proposal.units.size(); ++index)
                positions[semantic_canonical_json(
                    semantic_meaning_unit_payload(proposal.units[index]))].push_back(index);
        }
        const std::set<std::string, std::less<>> unresolved(
            proposal.unresolved.begin(), proposal.unresolved.end());
        std::set<std::string, std::less<>> semantic_unresolved;
        try {
            const auto body = parse_semantic_response(*failure.response_utf8);
            if (!body.is_object() || body.as_object().size() != 2 ||
                !body.as_object().contains("units") ||
                !body.as_object().contains("unresolved") ||
                !body.at("unresolved").is_array())
                throw std::invalid_argument("unknown_partial_response_fields");
            for (const auto& value : body.at("unresolved").as_array()) {
                const auto* anchor = std::get_if<std::string>(&value.storage());
                if (!anchor || !unresolved.contains(*anchor) ||
                    !semantic_unresolved.insert(*anchor).second)
                    throw std::invalid_argument("invalid_partial_unresolved_declaration");
            }
        } catch (const std::exception&) {
            semantic_unresolved = unresolved;
        }
        for (const auto& unit : units) {
            const auto key = semantic_canonical_json(semantic_meaning_unit_payload(unit));
            auto& matches = positions[key];
            const auto references = unit_references(unit);
            const bool all_unresolved = std::ranges::all_of(
                references, [&](const auto& anchor) { return unresolved.contains(anchor); });
            if (matches.empty() || !all_unresolved)
                throw std::invalid_argument("partial_semantic_units_changed");
            const auto index = matches.front();
            matches.pop_front();
            if (std::ranges::none_of(references, [&](const auto& anchor) {
                    return semantic_unresolved.contains(anchor);
                }))
                eligible[proposal.source_id].insert(index);
        }
    }
    for (const auto& proposal : proposals)
        for (const auto index : proposal.partial_response_units)
            if (!eligible[proposal.source_id].contains(index))
                throw std::invalid_argument(
                    "partial_response_scope_not_derived_from_failure");
    return eligible;
}

std::string error_name(const std::exception& error) {
    if (dynamic_cast<const SemanticResponseError*>(&error)) return "SemanticResponseError";
    if (dynamic_cast<const std::invalid_argument*>(&error)) return "ValueError";
    if (dynamic_cast<const std::out_of_range*>(&error)) return "IndexError";
    if (dynamic_cast<const std::runtime_error*>(&error)) return "RuntimeError";
    return "Exception";
}

}  // namespace

SemanticPreparationFailure::SemanticPreparationFailure(
    std::string source_id_value, std::string source_revision_value,
    std::string error_type_value, std::optional<std::string> response_utf8_value,
    const std::int64_t elapsed_ns_value, const bool proposal_created_value,
    std::optional<std::string> failure_code_value,
    std::optional<std::string> finish_reason_value,
    const std::size_t partial_unit_count_value)
    : source_id(std::move(source_id_value)),
      source_revision(std::move(source_revision_value)),
      error_type(std::move(error_type_value)),
      response_utf8(std::move(response_utf8_value)), elapsed_ns(elapsed_ns_value),
      proposal_created(proposal_created_value),
      failure_code(std::move(failure_code_value)),
      finish_reason(std::move(finish_reason_value)),
      partial_unit_count(partial_unit_count_value) {
    if (partial_unit_count &&
        (failure_code != "semantic_response_incomplete" ||
         finish_reason != "length" || !response_utf8 || !proposal_created))
        throw std::invalid_argument("invalid_partial_unit_failure");
    if (!failure_code && !finish_reason) return;
    static const std::set<std::string, std::less<>> codes{
        "semantic_transport_failed", "semantic_response_incomplete",
        "semantic_response_invalid_envelope", "semantic_text_proposal_required"};
    static const std::set<std::string, std::less<>> reasons{
        "stop", "length", "tool_calls", "function_call",
        "content_filter", "missing", "other"};
    if (!failure_code || !finish_reason || !codes.contains(*failure_code) ||
        !reasons.contains(*finish_reason))
        throw std::invalid_argument("invalid_semantic_failure_diagnostic");
}

JsonValue::Object SemanticPreparationFailure::receipt() const {
    JsonValue response;
    if (response_utf8) response = *response_utf8;
    JsonValue::Object result{
        {"source_id", source_id}, {"source_revision", source_revision},
        {"error_type", error_type}, {"response", std::move(response)},
        {"elapsed_ns", elapsed_ns},
        {"retained_as_unresolved_interpretation", proposal_created},
        {"original_retained", true}, {"proposal_created", proposal_created},
        {"retries", 0}, {"grants_authority", false},
    };
    if (failure_code)
        result.emplace("producer_diagnostic", JsonValue::Object{
            {"code", *failure_code}, {"finish_reason", *finish_reason}});
    if (partial_unit_count)
        result.emplace("partial_unit_count",
                       static_cast<std::int64_t>(partial_unit_count));
    return result;
}

SemanticPreparationFailure SemanticPreparationFailure::from_receipt(
    const JsonValue::Object& row) {
    const auto* diagnostic_value = find(row, "producer_diagnostic");
    std::optional<std::string> code;
    std::optional<std::string> reason;
    if (diagnostic_value) {
        if (!diagnostic_value->is_object() ||
            diagnostic_value->as_object().size() != 2 ||
            !diagnostic_value->as_object().contains("code") ||
            !diagnostic_value->as_object().contains("finish_reason"))
            throw std::invalid_argument("invalid_semantic_failure_diagnostic");
        code = text(diagnostic_value->as_object(), "code");
        reason = text(diagnostic_value->as_object(), "finish_reason");
    }
    std::optional<std::string> response;
    if (const auto* value = find(row, "response")) {
        if (const auto* string = std::get_if<std::string>(&value->storage())) response = *string;
        else if (!std::holds_alternative<std::nullptr_t>(value->storage()))
            throw std::invalid_argument("semantic_failure_receipt_changed");
    } else throw std::invalid_argument("semantic_failure_receipt_changed");
    const auto partial_value = find(row, "partial_unit_count");
    const auto partial = partial_value ? integer(row, "partial_unit_count") : 0;
    if (partial < 0) throw std::invalid_argument("semantic_failure_receipt_changed");
    SemanticPreparationFailure result(
        text(row, "source_id"), text(row, "source_revision"),
        text(row, "error_type"), std::move(response), integer(row, "elapsed_ns"),
        boolean(row, "proposal_created"), std::move(code), std::move(reason),
        static_cast<std::size_t>(partial));
    if (JsonValue(result.receipt()) != JsonValue(row))
        throw std::invalid_argument("semantic_failure_receipt_changed");
    return result;
}

std::vector<SemanticEncoding> PreparedSemanticBatch::for_wave(
    const std::vector<SemanticSourceEpisode>& episodes) const {
    std::vector<std::string> identifiers;
    identifiers.reserve(episodes.size());
    for (const auto& episode : episodes) identifiers.push_back(episode.episode_id);
    std::vector<std::string> proposed;
    std::set<std::string, std::less<>> proposed_set;
    for (const auto& proposal : proposals) {
        proposed.push_back(proposal.source_id);
        proposed_set.insert(proposal.source_id);
    }
    std::set<std::string, std::less<>> unencoded;
    for (const auto& failure : failures)
        if (!failure.proposal_created) unencoded.insert(failure.source_id);
    std::set<std::string, std::less<>> missing;
    std::vector<std::string> ordered;
    for (const auto& identifier : identifiers)
        if (proposed_set.contains(identifier)) ordered.push_back(identifier);
        else missing.insert(identifier);
    if (identifiers != source_ids || proposed_set.size() != proposed.size() ||
        proposed != ordered || unencoded != missing)
        throw std::invalid_argument("prepared_semantic_batch_wave_changed");
    (void)validate_retained_units(proposals, failures);
    return proposals;
}

PreparedSemanticBatch classify_partial_response_units(
    const PreparedSemanticBatch& batch) {
    auto result = batch;
    const auto eligible = validate_retained_units(result.proposals, result.failures);
    for (auto& proposal : result.proposals) {
        proposal.partial_response_units.assign(
            eligible.at(proposal.source_id).begin(), eligible.at(proposal.source_id).end());
    }
    return result;
}

PreparedSemanticBatch retain_complete_length_units(
    const PreparedSemanticBatch& batch) {
    auto result = batch;
    std::map<std::string, std::size_t, std::less<>> indexes;
    for (std::size_t index = 0; index < result.proposals.size(); ++index)
        indexes.emplace(result.proposals[index].source_id, index);
    for (auto& failure : result.failures) {
        if (failure.failure_code != "semantic_response_incomplete" ||
            failure.finish_reason != "length" || !failure.response_utf8 ||
            !failure.proposal_created || !indexes.contains(failure.source_id) ||
            failure.partial_unit_count) continue;
        auto& proposal = result.proposals[indexes.at(failure.source_id)];
        const std::set<std::string, std::less<>> unresolved(
            proposal.unresolved.begin(), proposal.unresolved.end());
        std::set<std::string, std::less<>> anchors;
        for (const auto& anchor : proposal.anchors) anchors.insert(anchor.identifier);
        if (!proposal.units.empty() ||
            proposal.source_revision != failure.source_revision ||
            unresolved != anchors) continue;
        auto units = complete_semantic_unit_prefix(*failure.response_utf8,
                                                   proposal.anchors);
        if (!units.empty()) {
            proposal.units = std::move(units);
            failure.partial_unit_count = proposal.units.size();
        }
    }
    return result;
}

PreparedSemanticBatch prepare_semantic_batch(
    const std::vector<SemanticSourceEpisode>& episodes,
    const SemanticPartsAdapter& parts_for,
    std::string model,
    const SemanticProducer& producer) {
    const auto began = now_ns();
    std::vector<std::string> identifiers;
    std::set<std::string, std::less<>> unique;
    for (const auto& episode : episodes) {
        identifiers.push_back(episode.episode_id);
        unique.insert(episode.episode_id);
    }
    if (identifiers.empty() || unique.size() != identifiers.size())
        throw std::invalid_argument("distinct_nonempty_semantic_wave_required");
    std::vector<SemanticEncoding> proposals;
    std::vector<SemanticPreparationFailure> failures;
    for (const auto& source : episodes) {
        const auto started = now_ns();
        std::vector<SemanticDeliveredPart> parts;
        try {
            parts = parts_for(source);
        } catch (const SourceAdapterUnavailable&) {
            failures.emplace_back(source.episode_id, source.revision,
                "SourceAdapterUnavailable", std::nullopt, now_ns() - started, false);
            continue;
        }
        bool called{};
        std::optional<std::string> response;
        std::vector<SemanticSourceContext> delivered_context;
        const SemanticProducer observed = [&](const SemanticEncodingInput& request) {
            called = true;
            delivered_context = request.source_context;
            auto body = producer(request);
            response = std::holds_alternative<std::string>(body)
                ? std::get<std::string>(body)
                : semantic_canonical_json(std::get<JsonValue>(body));
            return body;
        };
        try {
            proposals.push_back(encode_semantic_offline(
                source, parts, model, observed));
        } catch (const ProviderCancelled&) {
            throw;
        } catch (const std::exception& error) {
            if (!called) throw;
            const auto* typed = dynamic_cast<const SemanticResponseError*>(&error);
            if (typed) response = typed->response_utf8;
            std::vector<SemanticAnchor> anchors;
            anchors.reserve(parts.size());
            for (const auto& part : parts) anchors.push_back(part.anchor);
            std::vector<std::string> unresolved;
            for (const auto& anchor : anchors) unresolved.push_back(anchor.identifier);
            std::vector<std::string> outcomes;
            for (const auto& step : source.steps) outcomes.push_back(step.outcome);
            JsonValue::Array context;
            for (const auto& row : delivered_context) context.push_back(row.to_json());
            proposals.push_back({
                source.episode_id, source.revision, semantic_source_digest(source),
                source.source_addresses, std::move(outcomes), model,
                std::move(anchors), {}, std::move(unresolved), {}, std::move(context)});
            failures.emplace_back(
                source.episode_id, source.revision, error_name(error),
                std::move(response), now_ns() - started, true,
                typed ? std::optional<std::string>(typed->failure_code) : std::nullopt,
                typed ? std::optional<std::string>(typed->finish_reason) : std::nullopt);
        }
    }
    return {std::move(identifiers), std::move(proposals), std::move(failures),
            now_ns() - began};
}

}  // namespace swegca::world
