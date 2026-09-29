#include "world/utterance_receipt.hpp"

#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace swegca::world {

const std::string model_receipt_guide = R"(
utterance_plan (rozephine-utterance-plan-v1) declares the immutable main-owned sequence in advance.
utterance_part (rozephine-utterance-receipt-v1) is the prepared receipt for this call, not a task to fill in.
request_id, view_id and pair_snapshot_id bind one request incarnation and snapshot.
plan_id binds the whole answer; part_id identifies this part. part_index is zero-based;
part_count is the fixed total. Previous and next parts follow that index in plan.parts.
content_refs are source-bound main claim addresses, not additional content to retrieve.
Express only the current part and its qualifications; do not repeat earlier parts or anticipate later ones.
Main writes prepared/delivered/failed/cancelled events. delivered_parts and remaining_part_ids
track delivery, while complete means all parts were delivered, not that their wording is verified.
Return expression text only: never emit, acknowledge, forge or alter receipts, IDs, state or completion.
Receipts are delivery bookkeeping, not new evidence, truth certification or action authority.
The delivery layer joins ordered deltas once, without a synthesis model. Do not select, reorder,
cancel, retry or synthesize subsequent parts, request tools, or change main's content or decisions.
)";

namespace {

const JsonValue* find(const JsonValue::Object& object, const std::string_view key) {
    const auto item = object.find(key);
    return item == object.end() ? nullptr : &item->second;
}

bool string_is(const JsonValue::Object& object, const std::string_view key,
               const std::string_view expected) {
    const auto* value = find(object, key);
    const auto* text = value ? std::get_if<std::string>(&value->storage()) : nullptr;
    return text && *text == expected;
}

std::optional<std::int64_t> integer(const JsonValue::Object& object,
                                    const std::string_view key) {
    const auto* value = find(object, key);
    if (!value) return std::nullopt;
    if (const auto* number = std::get_if<std::int64_t>(&value->storage())) return *number;
    return std::nullopt;
}

bool boolean_is(const JsonValue::Object& object, const std::string_view key,
                const bool expected) {
    const auto* value = find(object, key);
    const auto* boolean = value ? std::get_if<bool>(&value->storage()) : nullptr;
    return boolean && *boolean == expected;
}

JsonValue::Array string_array(const std::vector<std::string>& values) {
    JsonValue::Array result;
    result.reserve(values.size());
    for (const auto& value : values) result.emplace_back(value);
    return result;
}

}  // namespace

std::size_t validate_delivered_part(
    const JsonValue::Object& receipt,
    const std::string_view request_id,
    const std::string_view view_id,
    const std::string_view pair_snapshot_id,
    const std::size_t next_index,
    const std::optional<std::size_t> part_count) {
    const std::string plan = "final-utterance:" + std::string(view_id);
    const std::string part = plan + ":" + std::to_string(next_index);
    const auto count = integer(receipt, "part_count");
    const auto index = integer(receipt, "part_index");
    const auto* delta_value = find(receipt, "delta");
    const auto* delta = delta_value
        ? std::get_if<std::string>(&delta_value->storage()) : nullptr;
    if (view_id.empty() ||
        !string_is(receipt, "schema", utterance_receipt_schema) ||
        !string_is(receipt, "owner", "main") ||
        !string_is(receipt, "state", "delivered") ||
        !string_is(receipt, "request_id", request_id) ||
        !string_is(receipt, "view_id", view_id) ||
        !string_is(receipt, "pair_snapshot_id", pair_snapshot_id) ||
        !string_is(receipt, "plan_id", plan) ||
        !string_is(receipt, "part_id", part) ||
        !string_is(receipt, "event_id", part + ":delivered") ||
        !count || *count <= static_cast<std::int64_t>(next_index) ||
        (part_count && *count != static_cast<std::int64_t>(*part_count)) ||
        !index || *index != static_cast<std::int64_t>(next_index) ||
        !delta || delta->empty())
        throw std::invalid_argument("utterance_delivery_binding_changed");
    return static_cast<std::size_t>(*count);
}

UtterancePlan::UtterancePlan(
    std::string request_id_value, std::string view_id_value,
    std::string pair_snapshot_id_value,
    std::vector<UtteranceClaimGroup> groups_value)
    : request_id(std::move(request_id_value)), view_id(std::move(view_id_value)),
      pair_snapshot_id(std::move(pair_snapshot_id_value)),
      groups(std::move(groups_value)) {
    if (request_id.empty() || view_id.empty() || pair_snapshot_id.empty())
        throw std::invalid_argument(
            "utterance plan requires request incarnation and snapshot identifiers");
    if (groups.empty())
        throw std::invalid_argument("nonempty immutable main content groups required");
    std::set<std::tuple<std::string, std::size_t, std::size_t>> seen;
    for (const auto& group : groups) {
        if (!group) {
            if (groups.size() != 1)
                throw std::invalid_argument(
                    "whole main answer cannot mix with partial groups");
            continue;
        }
        if (group->empty())
            throw std::invalid_argument("nonempty immutable claim group required");
        for (const auto& reference : *group) {
            if (reference.episode_id.empty() ||
                !seen.emplace(reference.episode_id, reference.step,
                              reference.claim).second)
                throw std::invalid_argument(
                    "each main content address belongs to one expression part");
        }
    }
}

std::string UtterancePlan::plan_id() const {
    return "final-utterance:" + view_id;
}

std::string UtterancePlan::part_id(const std::size_t index) const {
    if (index >= groups.size())
        throw std::invalid_argument("valid utterance part index required");
    return plan_id() + ":" + std::to_string(index);
}

JsonValue::Object UtterancePlan::prepared(const std::size_t index) const {
    const auto part = part_id(index);
    JsonValue::Array content_refs;
    if (groups[index]) {
        content_refs.reserve(groups[index]->size());
        for (const auto& reference : *groups[index]) {
            content_refs.emplace_back(JsonValue::Object{
                {"episode_id", reference.episode_id},
                {"step", static_cast<std::int64_t>(reference.step)},
                {"claim", static_cast<std::int64_t>(reference.claim)},
            });
        }
    }
    return {
        {"schema", std::string(utterance_receipt_schema)},
        {"owner", "main"}, {"request_id", request_id}, {"view_id", view_id},
        {"pair_snapshot_id", pair_snapshot_id}, {"plan_id", plan_id()},
        {"part_id", part}, {"event_id", part + ":prepared"},
        {"event_type", "part_prepared"},
        {"part_index", static_cast<std::int64_t>(index)},
        {"part_count", static_cast<std::int64_t>(groups.size())},
        {"state", "prepared"}, {"content_refs", std::move(content_refs)},
        {"content_mode", groups[index] ? "main_selected_claim_group" : "whole_main_answer"},
        {"main_order_fixed", true}, {"all_content_retained_in_main", true},
        {"previous_model_output_controls_content", false}, {"grants_authority", false},
    };
}

JsonValue::Object UtterancePlan::receipt() const {
    JsonValue::Array parts;
    parts.reserve(groups.size());
    for (std::size_t index = 0; index < groups.size(); ++index)
        parts.emplace_back(prepared(index));
    return {
        {"schema", std::string(utterance_plan_schema)}, {"owner", "main"},
        {"plan_id", plan_id()}, {"request_id", request_id}, {"view_id", view_id},
        {"pair_snapshot_id", pair_snapshot_id},
        {"part_count", static_cast<std::int64_t>(groups.size())},
        {"parts", std::move(parts)}, {"receipt_owner", "main"},
        {"model_returns", "expression_text_only"}, {"model_may_change_plan", false},
        {"model_synthesis_calls", 0}, {"grants_authority", false},
    };
}

JsonValue::Object UtterancePlan::delivered(
    const std::size_t index, const std::size_t delivered_parts,
    std::string delta, const bool failed,
    const JsonValue::Object* request_receipt,
    const std::int64_t started_ns, const std::int64_t finished_ns) const {
    if (delivered_parts != index + static_cast<std::size_t>(!failed) ||
        (failed && !delta.empty()) || (!failed && delta.empty()) ||
        started_ns < 0 || finished_ns < 0 || finished_ns < started_ns)
        throw std::invalid_argument(
            "consistent main delivery outcome and monotonic timestamps required");
    auto result = prepared(index);
    const std::string state = failed ? "failed" : "delivered";
    std::vector<std::string> remaining;
    for (std::size_t at = delivered_parts; at < groups.size(); ++at)
        remaining.push_back(part_id(at));
    JsonValue detached_sha;
    if (request_receipt) {
        if (const auto* value = find(*request_receipt, "request_sha256"))
            detached_sha = *value;
    }
    result.insert_or_assign("event_id", part_id(index) + ":" + state);
    result.insert_or_assign("event_type", "part_" + state);
    result.insert_or_assign("state", state);
    result.insert_or_assign("delivered_parts", static_cast<std::int64_t>(delivered_parts));
    result.insert_or_assign("remaining_parts", static_cast<std::int64_t>(groups.size() - delivered_parts));
    result.insert_or_assign("remaining_part_ids", string_array(remaining));
    result.insert_or_assign("complete", !failed && delivered_parts == groups.size());
    result.insert_or_assign("delta", std::move(delta));
    result.insert_or_assign("terminal_failure", failed);
    result.insert_or_assign("model_synthesis_calls", 0);
    result.insert_or_assign("detached_request_sha256", std::move(detached_sha));
    result.insert_or_assign("clock", "perf_counter_ns");
    result.insert_or_assign("part_started_ns", started_ns);
    result.insert_or_assign("part_finished_ns", finished_ns);
    result.insert_or_assign("model_wording_semantically_verified", false);
    return result;
}

JsonValue::Object cancelled_delivery(const JsonValue::Object& receipt) {
    const bool prepared = string_is(receipt, "state", "prepared");
    const bool delivered = string_is(receipt, "state", "delivered");
    if (!string_is(receipt, "schema", utterance_receipt_schema) ||
        (!prepared && !delivered) || boolean_is(receipt, "complete", true))
        throw std::invalid_argument(
            "an incomplete delivered checkpoint is required for cancellation");
    auto result = receipt;
    const auto count = integer(result, "part_count");
    const auto* plan_value = find(result, "plan_id");
    const auto* plan = plan_value
        ? std::get_if<std::string>(&plan_value->storage()) : nullptr;
    if (!count || *count < 0 || !plan)
        throw std::invalid_argument(
            "an incomplete delivered checkpoint is required for cancellation");
    if (!delivered) {
        std::vector<std::string> remaining;
        remaining.reserve(static_cast<std::size_t>(*count));
        for (std::int64_t index = 0; index < *count; ++index)
            remaining.push_back(*plan + ":" + std::to_string(index));
        result.insert_or_assign("delivered_parts", 0);
        result.insert_or_assign("remaining_parts", *count);
        result.insert_or_assign("remaining_part_ids", string_array(remaining));
        result.insert_or_assign("model_synthesis_calls", 0);
        result.insert_or_assign("model_wording_semantically_verified", false);
    }
    result.insert_or_assign("event_id", *plan + ":cancelled");
    result.insert_or_assign("event_type", "sequence_cancelled");
    result.insert_or_assign("state", "cancelled");
    result.insert_or_assign("delta", "");
    result.insert_or_assign("complete", false);
    result.insert_or_assign("cancelled", true);
    result.insert_or_assign("refers_to_last_delivered_part", delivered);
    return result;
}

}  // namespace swegca::world
