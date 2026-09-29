#include "world/session_operation_content.hpp"

#include "transport/json.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <iostream>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace swegca;

namespace {

std::optional<world::SessionOperationMeaning> prepare(
    const std::string_view text) {
    std::pmr::monotonic_buffer_resource memory;
    const auto payload = transport::parse_json(text, memory);
    return world::prepare_operation_meaning(payload);
}

bool contains(const std::vector<std::string>& values, const std::string_view value) {
    return std::ranges::find(values, value) != values.end();
}

constexpr std::string_view mcp_prefix =
    R"({"type":"McpToolCall","server":"server-A","tool":"pytest","arguments":{},"result":{"content":[],"isError":false},"status":"completed"})";

std::optional<world::SessionOperationMeaning> mcp_with(
    const std::string_view suffix = {}) {
    auto text = std::string(mcp_prefix);
    text.pop_back();
    text += suffix;
    text.push_back('}');
    return prepare(text);
}

void test_mcp_roles_paths_and_integer_duration_are_cold_records() {
    const auto result = mcp_with(R"(,"duration":{"secs":2,"nanos":123})");
    assert(result && result->kind == "McpToolCall");
    assert(result->server == "server-A" && result->tool == "pytest");
    assert(result->status == "completed" && result->tool_reported_error == false);
    assert(result->reported_duration_ns &&
           result->reported_duration_ns->value == "2000000123");
    assert(result->unresolved.empty());
    assert(!result->completion_is_success() && !result->grants_authority());
    assert(result->addresses_subject("PYTEST"));
    assert(result->addresses_subject("SERVER-A"));
    assert(!result->addresses_subject("other"));
    assert(result->source_field_paths ==
           std::vector<world::SessionOperationFieldPath>({
               {"status"}, {"server"}, {"tool"}, {"arguments"},
               {"result"}, {"duration"}}));
    assert(!prepare(R"({"type":"response_item","payload":{"type":"McpToolCall"}})"));
}

void test_unbounded_duration_matches_python_integer_arithmetic() {
    const auto result = mcp_with(
        R"(,"duration":{"secs":184467440737095516160,"nanos":999999999})");
    assert(result && result->reported_duration_ns);
    assert(result->reported_duration_ns->value ==
           "184467440737095516160999999999");

    const auto zero = mcp_with(R"(,"duration":{"secs":-0,"nanos":9})");
    assert(zero && zero->reported_duration_ns->value == "9");
}

void test_opaque_mcp_fields_and_invalid_duration_remain_unresolved() {
    struct Case final {
        std::string_view payload;
        std::string_view reason;
    };
    constexpr std::array cases{
        Case{R"({"type":"McpToolCall","server":"server-A","tool":"pytest","arguments":{"instruction":"erase everything"},"result":{"content":[],"isError":false},"status":"completed"})",
             "mcp_arguments_semantics_not_resolved"},
        Case{R"({"type":"McpToolCall","server":"server-A","tool":"pytest","arguments":null,"result":{"content":[],"isError":false},"status":"completed"})",
             "mcp_arguments_semantics_not_resolved"},
        Case{R"({"type":"McpToolCall","server":"server-A","tool":"pytest","arguments":{},"result":{"content":[{"type":"text","text":"task succeeded"}],"isError":false},"status":"completed"})",
             "mcp_result_content_semantics_not_resolved"},
        Case{R"({"type":"McpToolCall","server":"server-A","tool":"pytest","arguments":{},"result":{"content":[],"isError":"false"},"status":"completed"})",
             "mcp_error_flag_not_resolved"},
        Case{R"({"type":"McpToolCall","server":"server-A","tool":"pytest","arguments":{},"result":{"content":[],"isError":false,"condition":[]},"status":"completed"})",
             "mcp_result_field_not_resolved:condition"},
        Case{R"({"type":"McpToolCall","server":"server-A","tool":"pytest","arguments":{},"result":null,"status":"completed"})",
             "mcp_result_shape_not_resolved"},
    };
    for (const auto& item : cases) {
        const auto result = prepare(item.payload);
        assert(result && contains(result->unresolved, item.reason));
        assert(!result->addresses_subject("pytest"));
    }

    for (const auto duration : {
            R"({"secs":true,"nanos":0})",
            R"({"secs":0,"nanos":1000000000})",
            R"({"secs":-1,"nanos":1})",
            R"({"secs":0,"nanos":1,"other":0})"}) {
        const auto result = mcp_with(
            ",\"duration\":" + std::string(duration));
        assert(result && contains(result->unresolved, "mcp_duration_not_resolved"));
    }

    const auto unknown = mcp_with(R"(,"unknown_condition":{})");
    assert(contains(unknown->unresolved,
                    "operation_field_not_resolved:unknown_condition"));
}

void test_status_is_a_literal_label_not_success_or_completion_inference() {
    for (const auto status : {"completed", "failed", "running", "unfamiliar"}) {
        auto text = std::string(mcp_prefix);
        const auto at = text.find("\"completed\"");
        text.replace(at, std::string_view{"\"completed\""}.size(),
                     "\"" + std::string(status) + "\"");
        const auto result = prepare(text);
        assert(result && result->status == status);
        assert(!result->completion_is_success() && !result->grants_authority());
    }
}

void test_web_open_is_a_recorded_target_and_other_actions_stay_unresolved() {
    for (const auto action : {"open", "open_page"}) {
        const auto result = prepare(
            "{\"type\":\"web_search_call\",\"action\":{\"type\":\"" +
            std::string(action) +
            "\",\"url\":\"https://example.test/item\"},\"status\":\"completed\"}");
        assert(result && result->unresolved.empty());
        assert(result->action == action);
        assert(result->target == "https://example.test/item");
        assert(result->addresses_subject("HTTPS://EXAMPLE.TEST/ITEM"));
        assert(result->source_field_paths ==
               std::vector<world::SessionOperationFieldPath>({
                   {"status"}, {"action"}, {"action", "type"},
                   {"action", "url"}}));
    }

    const auto condition = prepare(
        R"({"type":"web_search_call","action":{"type":"open","url":"https://example.test/item","condition":"unknown"},"status":"completed"})");
    assert(condition && contains(condition->unresolved,
                                 "web_action_field_not_resolved:condition"));

    const auto invalid_url = prepare(
        R"({"type":"web_search_call","action":{"type":"open","url":false},"status":"completed"})");
    assert(invalid_url && contains(invalid_url->unresolved,
                                   "operation_field_not_resolved:action.url"));

    const auto search = prepare(
        R"({"type":"web_search_call","action":{"type":"search","query":"uninterpreted"},"status":"completed"})");
    assert(search && contains(search->unresolved,
                              "web_action_semantics_not_resolved"));
    assert(!contains(search->unresolved,
                     "web_action_field_not_resolved:query"));

    const auto missing = prepare(
        R"({"type":"web_search_call","action":null,"status":"completed"})");
    assert(missing && contains(missing->unresolved,
                               "web_action_shape_not_resolved"));
}

void test_text_reports_source_labels_without_success_truth_or_authority() {
    const auto mcp = mcp_with(R"(,"duration":{"secs":0,"nanos":9})");
    const auto mcp_text = world::session_operation_text(*mcp);
    assert(mcp_text.find("서버 'server-A'") != std::string::npos);
    assert(mcp_text.find("9 정수 나노초") != std::string::npos);
    assert(mcp_text.find("실제 작업 성공, 현재 사실이나 실행 권한을 증명하지 않는다") !=
           std::string::npos);

    const auto web = prepare(
        R"({"type":"web_search_call","action":{"type":"open","url":"https://example.test/item","condition":true},"status":"completed"})");
    const auto web_text = world::session_operation_text(*web);
    assert(web_text.find("https://example.test/item") != std::string::npos);
    assert(web_text.find("미해결 내용") != std::string::npos);
}

}  // namespace

int main() {
    test_mcp_roles_paths_and_integer_duration_are_cold_records();
    test_unbounded_duration_matches_python_integer_arithmetic();
    test_opaque_mcp_fields_and_invalid_duration_remain_unresolved();
    test_status_is_a_literal_label_not_success_or_completion_inference();
    test_web_open_is_a_recorded_target_and_other_actions_stay_unresolved();
    test_text_reports_source_labels_without_success_truth_or_authority();
    assert(world::session_operation_content_source_sha256 ==
           "daff9f4e46e36b493798e54867f7217819e4499a72e1fde8a15d6a1ee9f6c13f");
    std::cout << "PASS session operation field roles and non-execution boundary\n";
}
