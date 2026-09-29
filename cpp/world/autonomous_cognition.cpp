#include "world/autonomous_cognition.hpp"

#include "world/unicode_nfkc.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace swegca::world {
namespace {

bool has_text(const std::string_view value) {
    return !strip_unicode_whitespace(value).empty();
}

void require_text(const std::string_view value, const char* name) {
    if (!has_text(value)) {
        throw std::invalid_argument(std::string(name) + " must be nonempty");
    }
}

std::vector<std::string> deduplicate(std::vector<std::string> values) {
    std::set<std::string, std::less<>> seen;
    std::vector<std::string> result;
    result.reserve(values.size());
    for (auto& value : values) {
        if (seen.insert(value).second) result.push_back(std::move(value));
    }
    return result;
}

bool contains(const std::vector<std::string>& values, const std::string_view target) {
    return std::find(values.begin(), values.end(), target) != values.end();
}

const JsonValue* optional(const JsonValue::Object& object, const std::string_view key) {
    const auto found = object.find(key);
    return found == object.end() ? nullptr : &found->second;
}

bool is_null(const JsonValue& value) {
    return std::holds_alternative<std::nullptr_t>(value.storage());
}

std::optional<std::string> optional_string(const JsonValue::Object& object,
                                           const std::string_view key) {
    const auto* value = optional(object, key);
    if (value == nullptr || is_null(*value)) return std::nullopt;
    return std::string(value->as_string());
}

bool optional_bool(const JsonValue::Object& object, const std::string_view key,
                   const bool fallback) {
    const auto* value = optional(object, key);
    if (value == nullptr) return fallback;
    if (const auto* result = std::get_if<bool>(&value->storage())) return *result;
    throw std::invalid_argument("JSON value is not boolean");
}

std::int64_t python_int(const JsonValue& value) {
    if (const auto* integer = std::get_if<std::int64_t>(&value.storage())) return *integer;
    if (const auto* boolean = std::get_if<bool>(&value.storage())) return *boolean ? 1 : 0;
    if (const auto* number = std::get_if<double>(&value.storage())) {
        if (!std::isfinite(*number) || *number < static_cast<double>(std::numeric_limits<std::int64_t>::min()) ||
            *number > static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
            throw std::invalid_argument("JSON number cannot be converted to integer");
        }
        return static_cast<std::int64_t>(*number);
    }
    if (const auto* text = std::get_if<std::string>(&value.storage())) {
        std::int64_t result = 0;
        const auto [end, error] = std::from_chars(text->data(), text->data() + text->size(), result);
        if (error == std::errc{} && end == text->data() + text->size()) return result;
    }
    throw std::invalid_argument("JSON value cannot be converted to integer");
}

std::int64_t object_integer(const JsonValue::Object& object, const std::string_view key,
                            const std::int64_t fallback) {
    const auto* value = optional(object, key);
    return value == nullptr ? fallback : python_int(*value);
}

bool nonempty_value(const JsonValue::Object& object, const std::string_view key) {
    const auto* value = optional(object, key);
    if (value == nullptr || is_null(*value)) return false;
    if (const auto* boolean = std::get_if<bool>(&value->storage())) return *boolean;
    if (const auto* integer = std::get_if<std::int64_t>(&value->storage())) return *integer != 0;
    if (const auto* number = std::get_if<double>(&value->storage())) return *number != 0.0;
    if (const auto* text = std::get_if<std::string>(&value->storage())) return !text->empty();
    if (const auto* array = std::get_if<JsonValue::Array>(&value->storage())) return !array->empty();
    if (const auto* nested = std::get_if<JsonValue::Object>(&value->storage())) return !nested->empty();
    return false;
}

AutonomyEventKind expected_event(const AutonomyPhase phase) {
    switch (phase) {
    case AutonomyPhase::observe: return AutonomyEventKind::observation;
    case AutonomyPhase::hypothesize: return AutonomyEventKind::hypothesis;
    case AutonomyPhase::request_evidence: return AutonomyEventKind::evidence_request;
    case AutonomyPhase::collect_evidence: return AutonomyEventKind::evidence_action;
    case AutonomyPhase::observe_evidence_result: return AutonomyEventKind::evidence_result;
    case AutonomyPhase::verify: return AutonomyEventKind::verification;
    case AutonomyPhase::remember: return AutonomyEventKind::memory_commit;
    case AutonomyPhase::act: return AutonomyEventKind::action_proposal;
    case AutonomyPhase::observe_result: return AutonomyEventKind::action_result;
    case AutonomyPhase::abstain: return AutonomyEventKind::observation;
    }
    throw std::invalid_argument("unknown autonomy phase");
}

AutonomyPhase phase(const CognitiveState& state) {
    const auto found = state.goal_state().find("autonomy_phase");
    return found == state.goal_state().end()
        ? AutonomyPhase::observe
        : parse_autonomy_phase(found->second.as_string());
}

AutonomyTransition reject(const CognitiveState& state, const AutonomyPhase current,
                          std::string reason) {
    return {state, current, current, false, std::move(reason), false, false,
            std::nullopt, false, std::nullopt};
}

CognitiveState updated_state(const CognitiveState& state, const AutonomyEvent& event,
                             const AutonomyPhase next,
                             const JsonValue::Object& goal_updates = {},
                             const JsonValue::Object& self_updates = {}) {
    auto goal = state.goal_state();
    auto self = state.self_state();
    for (const auto& [key, value] : goal_updates) goal.insert_or_assign(key, value);
    for (const auto& [key, value] : self_updates) self.insert_or_assign(key, value);
    goal.insert_or_assign("autonomy_phase", std::string(autonomy_phase_name(next)));
    goal.insert_or_assign("autonomy_step", object_integer(goal, "autonomy_step", 0) + 1);
    goal.insert_or_assign("last_autonomy_event_id", event.event_id);
    return state.with_metadata(std::move(goal), std::move(self));
}

std::optional<std::string> payload_string(const JsonValue::Object& payload,
                                          const std::string_view key) {
    const auto* value = optional(payload, key);
    if (value == nullptr || is_null(*value)) return std::nullopt;
    if (const auto* result = std::get_if<std::string>(&value->storage())) return *result;
    return std::nullopt;
}

}  // namespace

std::string_view autonomy_phase_name(const AutonomyPhase phase) noexcept {
    switch (phase) {
    case AutonomyPhase::observe: return "observe";
    case AutonomyPhase::hypothesize: return "hypothesize";
    case AutonomyPhase::request_evidence: return "request_evidence";
    case AutonomyPhase::collect_evidence: return "collect_evidence";
    case AutonomyPhase::observe_evidence_result: return "observe_evidence_result";
    case AutonomyPhase::verify: return "verify";
    case AutonomyPhase::remember: return "remember";
    case AutonomyPhase::act: return "act";
    case AutonomyPhase::observe_result: return "observe_result";
    case AutonomyPhase::abstain: return "abstain";
    }
    return {};
}

AutonomyPhase parse_autonomy_phase(const std::string_view value) {
    for (const auto candidate : {AutonomyPhase::observe, AutonomyPhase::hypothesize,
                                 AutonomyPhase::request_evidence, AutonomyPhase::collect_evidence,
                                 AutonomyPhase::observe_evidence_result, AutonomyPhase::verify,
                                 AutonomyPhase::remember, AutonomyPhase::act,
                                 AutonomyPhase::observe_result, AutonomyPhase::abstain}) {
        if (autonomy_phase_name(candidate) == value) return candidate;
    }
    throw std::invalid_argument("unknown autonomy phase: " + std::string(value));
}

AutonomousCognitionConfig::AutonomousCognitionConfig(
    std::vector<std::string> allowed, std::vector<std::string> reversible,
    const double action_confidence, const std::uint64_t action_failures,
    std::vector<std::string> evidence_allowed,
    std::vector<std::string> evidence_reversible,
    const double evidence_confidence, const std::uint64_t evidence_failures)
    : allowed_tool_actions(deduplicate(std::move(allowed))),
      reversible_tool_actions(deduplicate(std::move(reversible))),
      minimum_action_confidence(action_confidence),
      maximum_action_failures(action_failures),
      allowed_evidence_tool_actions(deduplicate(std::move(evidence_allowed))),
      reversible_evidence_tool_actions(deduplicate(std::move(evidence_reversible))),
      minimum_evidence_action_confidence(evidence_confidence),
      maximum_evidence_action_failures(evidence_failures) {
    if (allowed_tool_actions.empty() ||
        std::any_of(allowed_tool_actions.begin(), allowed_tool_actions.end(),
                    [](const auto& value) { return !has_text(value); })) {
        throw std::invalid_argument("allowed tool actions must be nonempty");
    }
    for (const auto& value : reversible_tool_actions) {
        if (!contains(allowed_tool_actions, value)) {
            throw std::invalid_argument("reversible actions must be a subset of allowed actions");
        }
    }
    if (!std::isfinite(minimum_action_confidence) || minimum_action_confidence < 0.0 ||
        minimum_action_confidence > 1.0) {
        throw std::invalid_argument("minimum action confidence must be in [0, 1]");
    }
    if (maximum_action_failures == 0) {
        throw std::invalid_argument("maximum action failures must be positive");
    }
    if (std::any_of(allowed_evidence_tool_actions.begin(), allowed_evidence_tool_actions.end(),
                    [](const auto& value) { return !has_text(value); })) {
        throw std::invalid_argument("allowed evidence tool actions cannot be empty");
    }
    for (const auto& value : reversible_evidence_tool_actions) {
        if (!contains(allowed_evidence_tool_actions, value)) {
            throw std::invalid_argument(
                "reversible evidence actions must be a subset of allowed actions");
        }
    }
    if (!std::isfinite(minimum_evidence_action_confidence) ||
        minimum_evidence_action_confidence < 0.0 ||
        minimum_evidence_action_confidence > 1.0) {
        throw std::invalid_argument("minimum evidence action confidence must be in [0, 1]");
    }
    if (maximum_evidence_action_failures == 0) {
        throw std::invalid_argument("maximum evidence action failures must be positive");
    }
}

AutonomyEvent::AutonomyEvent(std::string id, const AutonomyEventKind event_kind,
                             std::optional<std::string> hypothesis,
                             std::vector<std::string> references,
                             std::string family, std::string context,
                             const double event_confidence, JsonValue::Object event_payload)
    : event_id(std::move(id)), kind(event_kind), hypothesis_id(std::move(hypothesis)),
      evidence_refs(std::move(references)), source_family(std::move(family)),
      context_hash(std::move(context)), confidence(event_confidence),
      payload(std::move(event_payload)) {
    require_text(event_id, "event_id");
    require_text(source_family, "source_family");
    require_text(context_hash, "context_hash");
    if (hypothesis_id && !has_text(*hypothesis_id)) {
        throw std::invalid_argument("hypothesis_id cannot be empty");
    }
    if (std::any_of(evidence_refs.begin(), evidence_refs.end(),
                    [](const auto& value) { return !has_text(value); })) {
        throw std::invalid_argument("evidence references cannot be empty");
    }
    if (!std::isfinite(confidence) || confidence < 0.0 || confidence > 1.0) {
        throw std::invalid_argument("event confidence must be in [0, 1]");
    }
}

AutonomyTransition advance_autonomous_cognition(
    const CognitiveState& state, const AutonomyEvent& event,
    const AutonomousCognitionConfig& config,
    const AccumulatorDecision* evidence_decision) {
    const auto current = phase(state);
    const auto last = optional_string(state.goal_state(), "last_autonomy_event_id");
    if (last && *last == event.event_id) return reject(state, current, "duplicate_event");

    if (event.kind == AutonomyEventKind::hypothesis_abandon) {
        if (current != AutonomyPhase::request_evidence &&
            current != AutonomyPhase::collect_evidence && current != AutonomyPhase::verify) {
            return reject(state, current, "hypothesis_not_abandonable");
        }
        if (event.hypothesis_id != optional_string(state.goal_state(), "active_hypothesis_id")) {
            return reject(state, current, "hypothesis_mismatch");
        }
        auto updated = updated_state(
            state, event, AutonomyPhase::abstain,
            {{"verification_status", "abandoned"},
             {"requested_evidence_axes", JsonValue::Array{}}});
        return {std::move(updated), current, AutonomyPhase::abstain, true,
                "hypothesis_abandoned", false, false, std::nullopt, false,
                std::nullopt};
    }
    if (event.kind != expected_event(current)) {
        return reject(state, current, "unexpected_event_for_phase");
    }

    const auto active_hypothesis = optional_string(state.goal_state(), "active_hypothesis_id");
    if (current != AutonomyPhase::observe && current != AutonomyPhase::abstain &&
        current != AutonomyPhase::hypothesize && event.hypothesis_id != active_hypothesis) {
        return reject(state, current, "hypothesis_mismatch");
    }

    auto next = current;
    std::string reason = "accepted";
    bool memory_write_allowed = false;
    bool tool_action_allowed = false;
    std::optional<std::string> tool_action;
    bool evidence_tool_action_allowed = false;
    std::optional<std::string> evidence_tool_action;
    JsonValue::Object goal_updates;
    JsonValue::Object self_updates;

    if (current == AutonomyPhase::observe || current == AutonomyPhase::abstain) {
        next = AutonomyPhase::hypothesize;
        goal_updates = {{"active_hypothesis_id", nullptr},
                        {"verified_hypothesis_id", nullptr},
                        {"memory_ref", nullptr}};
        self_updates = {{"autonomy_action_failures", 0},
                        {"autonomy_evidence_action_failures", 0}};
    } else if (current == AutonomyPhase::hypothesize) {
        if (!event.hypothesis_id || event.evidence_refs.empty()) {
            return reject(state, current, "hypothesis_requires_provenance");
        }
        next = AutonomyPhase::request_evidence;
        goal_updates = {{"active_hypothesis_id", *event.hypothesis_id},
                        {"hypothesis_confidence", event.confidence}};
    } else if (current == AutonomyPhase::request_evidence) {
        const auto* axes_value = optional(event.payload, "requested_axes");
        if (axes_value == nullptr || !axes_value->is_array() || axes_value->as_array().empty()) {
            return reject(state, current, "evidence_request_requires_axes");
        }
        for (const auto& axis : axes_value->as_array()) {
            if (!std::holds_alternative<std::string>(axis.storage()) || axis.as_string().empty()) {
                return reject(state, current, "evidence_request_requires_axes");
            }
        }
        bool collect_with_tool = false;
        try {
            collect_with_tool = optional_bool(event.payload, "collect_with_tool", false);
        } catch (const std::invalid_argument&) {
            return reject(state, current, "collect_with_tool_requires_boolean");
        }
        if (collect_with_tool && config.allowed_evidence_tool_actions.empty()) {
            return reject(state, current, "evidence_tool_collection_not_configured");
        }
        next = collect_with_tool ? AutonomyPhase::collect_evidence : AutonomyPhase::verify;
        goal_updates = {{"requested_evidence_axes", *axes_value}};
    } else if (current == AutonomyPhase::collect_evidence) {
        const auto action = payload_string(event.payload, "action");
        if (!action || !contains(config.allowed_evidence_tool_actions, *action)) {
            return reject(state, current, "evidence_action_not_allowed");
        }
        if (!contains(config.reversible_evidence_tool_actions, *action)) {
            return reject(state, current, "evidence_action_not_reversible");
        }
        if (event.confidence < config.minimum_evidence_action_confidence) {
            return reject(state, current, "evidence_action_confidence_too_low");
        }
        if (!nonempty_value(state.goal_state(), "requested_evidence_axes")) {
            return reject(state, current, "evidence_action_requires_requested_axes");
        }
        next = AutonomyPhase::observe_evidence_result;
        evidence_tool_action_allowed = true;
        evidence_tool_action = action;
        goal_updates = {{"pending_evidence_tool_action", *action}};
    } else if (current == AutonomyPhase::observe_evidence_result) {
        const auto* success_value = optional(event.payload, "success");
        if (success_value == nullptr || !std::holds_alternative<bool>(success_value->storage())) {
            return reject(state, current, "evidence_result_requires_boolean_success");
        }
        if (std::get<bool>(success_value->storage())) {
            next = AutonomyPhase::verify;
            goal_updates = {{"pending_evidence_tool_action", nullptr}};
            self_updates = {{"autonomy_evidence_action_failures", 0}};
        } else {
            const auto failures = object_integer(
                state.self_state(), "autonomy_evidence_action_failures", 0) + 1;
            next = static_cast<std::uint64_t>(failures) < config.maximum_evidence_action_failures
                ? AutonomyPhase::collect_evidence : AutonomyPhase::abstain;
            reason = next == AutonomyPhase::collect_evidence
                ? "recover_evidence_action" : "evidence_failure_budget_exhausted";
            goal_updates = {{"pending_evidence_tool_action", nullptr}};
            self_updates = {{"autonomy_evidence_action_failures", failures}};
        }
    } else if (current == AutonomyPhase::verify) {
        if (evidence_decision == nullptr) {
            return reject(state, current, "verification_requires_accumulator_decision");
        }
        if (evidence_decision->status == "accept") {
            next = AutonomyPhase::remember;
            memory_write_allowed = true;
            goal_updates = {{"verified_hypothesis_id",
                             event.hypothesis_id ? JsonValue(*event.hypothesis_id) : JsonValue(nullptr)},
                            {"verification_status", "accept"},
                            {"verification_lcb", evidence_decision->causal_lower_bound}};
        } else if (evidence_decision->status == "reject") {
            next = AutonomyPhase::abstain;
            reason = "hypothesis_rejected";
            goal_updates = {{"verification_status", "reject"}};
        } else {
            next = AutonomyPhase::request_evidence;
            reason = "additional_evidence_required";
            goal_updates = {{"verification_status", "abstain"}};
        }
    } else if (current == AutonomyPhase::remember) {
        const auto memory_ref = payload_string(event.payload, "memory_ref");
        const auto content_hash = payload_string(event.payload, "content_hash");
        if (event.hypothesis_id != optional_string(state.goal_state(), "verified_hypothesis_id") ||
            !memory_ref || memory_ref->empty() || !content_hash || content_hash->empty() ||
            event.evidence_refs.empty()) {
            return reject(state, current, "memory_commit_requires_verified_provenance");
        }
        next = AutonomyPhase::act;
        goal_updates = {{"memory_ref", *memory_ref}, {"memory_content_hash", *content_hash}};
    } else if (current == AutonomyPhase::act) {
        const auto action = payload_string(event.payload, "action");
        if (!action || !contains(config.allowed_tool_actions, *action)) {
            return reject(state, current, "action_not_allowed");
        }
        if (!contains(config.reversible_tool_actions, *action)) {
            return reject(state, current, "action_not_reversible");
        }
        if (event.confidence < config.minimum_action_confidence) {
            return reject(state, current, "action_confidence_too_low");
        }
        if (!nonempty_value(state.goal_state(), "memory_ref")) {
            return reject(state, current, "action_requires_verified_memory");
        }
        next = AutonomyPhase::observe_result;
        tool_action_allowed = true;
        tool_action = action;
        goal_updates = {{"pending_tool_action", *action}};
    } else if (current == AutonomyPhase::observe_result) {
        const auto* success_value = optional(event.payload, "success");
        if (success_value == nullptr || !std::holds_alternative<bool>(success_value->storage())) {
            return reject(state, current, "action_result_requires_boolean_success");
        }
        if (std::get<bool>(success_value->storage())) {
            next = AutonomyPhase::observe;
            goal_updates = {{"pending_tool_action", nullptr}};
            self_updates = {{"autonomy_action_failures", 0}};
        } else {
            const auto failures = object_integer(state.self_state(), "autonomy_action_failures", 0) + 1;
            next = static_cast<std::uint64_t>(failures) < config.maximum_action_failures
                ? AutonomyPhase::act : AutonomyPhase::abstain;
            reason = next == AutonomyPhase::act ? "recover_action" : "failure_budget_exhausted";
            goal_updates = {{"pending_tool_action", nullptr}};
            self_updates = {{"autonomy_action_failures", failures}};
        }
    }

    auto updated = updated_state(state, event, next, goal_updates, self_updates);
    return {std::move(updated), current, next, true, std::move(reason),
            memory_write_allowed, tool_action_allowed, std::move(tool_action),
            evidence_tool_action_allowed, std::move(evidence_tool_action)};
}

}  // namespace swegca::world
