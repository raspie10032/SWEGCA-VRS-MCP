#include "world/session_result_content.hpp"

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

std::optional<world::SessionResultMeaning> prepare(const std::string_view text) {
    std::pmr::monotonic_buffer_resource memory;
    const auto payload = transport::parse_json(text, memory);
    return world::prepare_result_meaning(payload);
}

bool contains(const std::vector<std::string>& values, const std::string_view value) {
    return std::ranges::find(values, value) != values.end();
}

void test_typed_fields_determine_only_the_recorded_process_state() {
    struct Case final {
        std::string_view output;
        std::string_view interpretation;
    };
    constexpr std::array cases{
        Case{R"({"exit_code":0})", "recorded_normal_exit"},
        Case{R"({"exit_code":-9})", "recorded_nonzero_exit"},
        Case{R"({"isError":true})", "tool_reported_error"},
        Case{R"({"session_id":42})", "recorded_running_handle_not_completion"},
        Case{R"({"exit_code":0,"session_id":42})", "ambiguous_process_state"},
        Case{R"({"isError":false})", "uninterpreted_result"},
        Case{R"({"exit_code":true})", "uninterpreted_result"},
        Case{R"({"exit_code":"0"})", "uninterpreted_result"},
    };
    for (const auto& item : cases) {
        const auto result = prepare(
            "{\"type\":\"custom_tool_call_output\",\"output\":" +
            std::string(item.output) + "}");
        assert(result && result->interpretation == item.interpretation);
        if (item.interpretation == "uninterpreted_result" ||
            item.interpretation == "ambiguous_process_state")
            assert(!result->unresolved.empty());
    }
}

void test_stdout_words_duplicate_keys_and_nonfinite_numbers_are_not_claims() {
    const auto prose = prepare(
        R"({"type":"custom_tool_call_output","output":"plain stdout says exit code 0"})");
    assert(prose && prose->interpretation == "uninterpreted_result");
    assert(prose->unresolved ==
           std::vector<std::string>({"unstructured_result_text"}));

    const auto duplicate = prepare(
        R"({"type":"custom_tool_call_output","output":"{\"exit_code\":0,\"exit_code\":1}"})");
    assert(duplicate && duplicate->unresolved ==
           std::vector<std::string>({"unstructured_result_text"}));

    const auto nonfinite = prepare(
        R"({"type":"custom_tool_call_output","output":"{\"exit_code\":0,\"measurement\":1e999}"})");
    assert(nonfinite && nonfinite->unresolved ==
           std::vector<std::string>({"unstructured_result_text"}));

    const auto underflow = prepare(
        R"({"type":"custom_tool_call_output","output":"{\"exit_code\":0,\"measurement\":1e-999}"})");
    assert(underflow && underflow->interpretation == "recorded_normal_exit");
    assert(underflow->unresolved ==
           std::vector<std::string>({"result_field_not_resolved:measurement"}));
}

void test_qualifications_and_output_fields_remain_explicit() {
    const auto result = prepare(
        R"({"type":"function_call_output","output":"{\"exit_code\":1,\"stdout\":\"unknown semantic qualification\",\"unknown\":false}"})");
    assert(result && result->exit_code && result->exit_code->value == "1");
    assert(result->unresolved == std::vector<std::string>({
        "result_field_not_resolved:stdout", "result_field_not_resolved:unknown"}));

    const auto empty = prepare(
        R"({"type":"function_call_output","output":{"exit_code":0,"output":"","stdout":"","stderr":""}})");
    assert(empty && empty->unresolved.empty());

    const auto qualified = prepare(
        R"({"type":"function_call_output","output":{"exit_code":0},"condition":"unknown"})");
    assert(qualified && qualified->unresolved ==
           std::vector<std::string>({"event_qualifications_not_resolved"}));
    assert(!qualified->interpretation.empty());
}

void test_integer_types_and_unbounded_values_follow_python_int_rules() {
    const auto large = prepare(
        R"({"type":"custom_tool_call_output","output":{"exit_code":-184467440737095516160,"session_id":184467440737095516160}})");
    assert(large && large->exit_code && large->process_session_id);
    assert(large->exit_code->value == "-184467440737095516160");
    assert(large->process_session_id->value == "184467440737095516160");
    assert(large->interpretation == "ambiguous_process_state");

    for (const auto value : {"0", "-1", "true", "1.0", R"("1")"}) {
        const auto result = prepare(
            "{\"type\":\"custom_tool_call_output\",\"output\":{\"session_id\":" +
            std::string(value) + "}}");
        assert(result && !result->process_session_id);
        assert(contains(result->unresolved, "invalid_process_session_type"));
        assert(contains(result->unresolved, "no_process_completion_evidence"));
    }

    const auto negative_zero = prepare(
        R"({"type":"custom_tool_call_output","output":{"exit_code":-0}})");
    assert(negative_zero && negative_zero->exit_code->value == "0");
    assert(negative_zero->interpretation == "recorded_normal_exit");
}

void test_unknown_shapes_and_unrelated_events_are_not_repaired() {
    for (const auto output : {"null", "false", "0", "[]"}) {
        const auto result = prepare(
            "{\"type\":\"custom_tool_call_output\",\"output\":" +
            std::string(output) + "}");
        assert(result && result->unresolved ==
               std::vector<std::string>({"unknown_result_shape"}));
    }
    const auto missing = prepare(R"({"type":"custom_tool_call_output"})");
    assert(missing && missing->unresolved ==
           std::vector<std::string>({"unknown_result_shape"}));
    assert(!prepare(R"({"type":"message","exit_code":0})"));
}

void test_rendering_preserves_request_result_and_authority_boundaries() {
    const auto meaning = prepare(
        R"({"type":"custom_tool_call_output","output":{"exit_code":1,"output":"secret prose"}})");
    assert(meaning);
    std::vector<world::SessionCallMeaning> calls;
    calls.emplace_back(std::optional<std::string>{"exec_command"},
        "requested_simple_command", std::optional<std::string>{"pytest"},
        std::vector<std::string>{"-q"}, std::vector<world::SessionCallOption>{},
        std::vector<std::string>{});
    const auto text = world::session_result_text(*meaning, calls);
    assert(text.find("pytest") != std::string::npos);
    assert(text.find("동일 session/turn/call ID") != std::string::npos);
    assert(text.find("비정상 종료") != std::string::npos);
    assert(text.find("전체 답이 완성되지는 않는다") != std::string::npos);
    assert(text.find("secret prose") == std::string::npos);

    const auto success = prepare(
        R"({"type":"custom_tool_call_output","output":{"exit_code":0}})");
    const auto success_text = world::session_result_text(*success);
    assert(success_text.find("작업의 실제 성공 증명은 아니다") != std::string::npos);
}

}  // namespace

int main() {
    test_typed_fields_determine_only_the_recorded_process_state();
    test_stdout_words_duplicate_keys_and_nonfinite_numbers_are_not_claims();
    test_qualifications_and_output_fields_remain_explicit();
    test_integer_types_and_unbounded_values_follow_python_int_rules();
    test_unknown_shapes_and_unrelated_events_are_not_repaired();
    test_rendering_preserves_request_result_and_authority_boundaries();
    assert(world::session_result_content_source_sha256 ==
           "f5747574eecc66c7e1738e1e61cd7cafca3e87dba658b7f3cb92a28f0b26c68d");
    std::cout << "PASS session result typed meaning and non-authoritative rendering\n";
}
