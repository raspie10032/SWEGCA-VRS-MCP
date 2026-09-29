#include "world/session_call_content.hpp"
#include "world/unicode_nfkc.hpp"

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

std::optional<world::SessionCallMeaning> prepare(const std::string_view text) {
    std::pmr::monotonic_buffer_resource memory;
    const auto payload = transport::parse_json(text, memory);
    return world::prepare_call_meaning(payload);
}

std::string quoted(const std::string_view value) {
    std::pmr::monotonic_buffer_resource memory;
    return std::string(transport::quote_json(value, memory));
}

std::optional<world::SessionCallMeaning> command(
    const std::string_view value, const std::string_view suffix = {}) {
    return prepare("{\"type\":\"custom_tool_call\",\"name\":\"exec_command\"," 
                   "\"input\":{\"cmd\":" + quoted(value) + std::string(suffix) + "}}");
}

bool contains(const std::vector<std::string>& values, const std::string_view value) {
    return std::ranges::find(values, value) != values.end();
}

void test_literal_request_is_parsed_without_execution_or_effect_claims() {
    const auto result = command("pytest -k \"unit case\"",
                                R"(,"workdir":"/work","login":false)");
    assert(result && result->operation == "requested_simple_command");
    assert(result->tool_name == "exec_command");
    assert(result->executable == "pytest");
    assert(result->arguments == std::vector<std::string>({"-k", "unit case"}));
    assert(result->requested_options == std::vector<world::SessionCallOption>({
        {"workdir", std::string{"/work"}}, {"login", false}}));
    assert(result->unresolved.empty());
    assert(result->addresses_subject("PYTEST"));
    assert(result->addresses_subject("EXEC_COMMAND"));
    assert(!result->addresses_subject("unit case"));

    const auto text = world::call_request_text(*result);
    assert(text.find("요청했다") != std::string::npos);
    assert(text.find("실제 대상 실행") != std::string::npos);
    assert(text.find("효과가 확인됐다는 뜻은 아니다") != std::string::npos);
}

void test_shell_programs_are_not_reported_as_one_literal_command() {
    const std::array<std::string_view, 17> programs{
        "pytest; echo yes", "pytest && echo yes", "pytest | tee out", "$(pytest)",
        "echo $HOME", "pytest > out", "pytest *.py", "X=1 pytest", "if true",
        "pytest\necho yes", "pytest \"unterminated", "", "\"\"",
        "pytest # comment", "pytest `echo yes`", "pytest <(echo yes)",
        std::string_view{"pytest\0echo", 11}};
    for (const auto program : programs) {
        const auto result = command(program);
        assert(result && !result->unresolved.empty());
        assert(!result->addresses_subject("pytest"));
    }
    const auto forbidden = command("pytest; echo yes");
    assert(forbidden->unresolved ==
           std::vector<std::string>({"shell_program_semantics_not_resolved"}));
    const auto empty_argv = command("\"\"");
    assert(empty_argv->unresolved ==
           std::vector<std::string>({"no_literal_command_target"}));
}

void test_options_are_typed_and_invalid_values_remain_unresolved() {
    const auto valid = command("pytest", R"(,"tty":true,"yield_time_ms":0,"max_output_tokens":184467440737095516160)");
    assert(valid && valid->unresolved.empty());
    assert(valid->requested_options.size() == 3);
    assert(std::get<bool>(valid->requested_options[0].value));
    assert(std::get<world::SessionDecimalInteger>(valid->requested_options[1].value).value == "0");
    assert(std::get<world::SessionDecimalInteger>(valid->requested_options[2].value).value ==
           "184467440737095516160");

    for (const auto suffix : {
            R"(,"unknown":true)", R"(,"tty":"yes")",
            R"(,"yield_time_ms":true)", R"(,"workdir":1)",
            R"(,"shell":"powershell")"}) {
        const auto result = command("pytest", suffix);
        assert(result && !result->unresolved.empty());
        assert(!result->addresses_subject("pytest"));
    }
    const auto shell = command("pytest", R"(,"shell":"powershell")");
    assert(contains(shell->unresolved, "call_option_not_resolved:shell") == false);
    assert(contains(shell->unresolved, "requested_shell_semantics_not_resolved"));
}

void test_function_arguments_duplicate_keys_and_nonfinite_numbers() {
    const auto function = prepare(
        R"({"type":"function_call","name":"functions.exec_command","arguments":"{\"cmd\":\"pytest\"}"})");
    assert(function && function->unresolved.empty());

    const auto duplicate = prepare(
        R"({"type":"function_call","name":"functions.exec_command","arguments":"{\"cmd\":\"pytest\",\"cmd\":\"echo\"}"})");
    assert(duplicate && duplicate->operation == "uninterpreted_call");
    assert(contains(duplicate->unresolved, "call_arguments_not_structured"));

    const auto nonfinite = prepare(
        R"({"type":"function_call","name":"functions.exec_command","arguments":"{\"cmd\":\"pytest\",\"max_output_tokens\":1e999}"})");
    assert(nonfinite && nonfinite->operation == "uninterpreted_call");
    assert(contains(nonfinite->unresolved, "call_arguments_not_structured"));

    const auto underflow = prepare(
        R"({"type":"function_call","name":"functions.exec_command","arguments":"{\"cmd\":\"pytest\",\"max_output_tokens\":1e-999}"})");
    assert(underflow && underflow->operation == "requested_simple_command");
    assert(underflow->unresolved ==
           std::vector<std::string>({"call_option_not_resolved:max_output_tokens"}));
    assert(!prepare(R"({"type":"message","content":"run pytest"})"));

    const auto huge_integer_arguments = std::string{"{\"cmd\":\"pytest\","
        "\"max_output_tokens\":"} + std::string(4301, '9') + "}";
    const auto huge_integer = prepare(
        "{\"type\":\"function_call\",\"name\":\"functions.exec_command\","
        "\"arguments\":" + quoted(huge_integer_arguments) + "}");
    assert(huge_integer && huge_integer->operation == "uninterpreted_call");
    assert(contains(huge_integer->unresolved, "call_arguments_not_structured"));
}

void test_python_json_number_spelling_for_source_hashes() {
    assert(transport::normalize_python_json_number("1e-400") == "0.0");
    assert(transport::normalize_python_json_number("-1e-400") == "-0.0");
    assert(transport::normalize_python_json_number("1.2345678901234568e16") ==
           "1.2345678901234568e+16");
    assert(transport::normalize_python_json_number("1e-4") == "0.0001");
}

void test_python_string_repr_spelling() {
    assert(world::python_string_repr("it's") == "\"it's\"");
    assert(world::python_string_repr("a\"b") == "'a\"b'");
    assert(world::python_string_repr(std::string_view{"\xc2\xa0", 2}) == "'\\xa0'");
    assert(world::python_string_repr(std::string_view{"\xe2\x80\x8b", 3}) == "'\\u200b'");
    assert(world::python_string_repr(std::string_view{"\xee\x80\x80", 3}) == "'\\ue000'");
    assert(world::python_string_repr(std::string_view{"\x01", 1}) == "'\\x01'");
}

void test_namespace_is_exact_recorded_identity() {
    const auto accepted = prepare(
        R"({"type":"custom_tool_call","name":"exec_command","namespace":"functions","input":{"cmd":"pytest"}})");
    assert(accepted && accepted->name_space == "functions" && accepted->unresolved.empty());
    assert(world::call_request_text(*accepted).find("namespace") != std::string::npos);

    for (const auto namespace_value : {
            R"("another")", R"(" functions ")", "null", "false", R"("")", "{}"}) {
        const auto result = prepare(
            "{\"type\":\"custom_tool_call\",\"name\":\"exec_command\",\"namespace\":" +
            std::string(namespace_value) + ",\"input\":{\"cmd\":\"pytest\"}}");
        assert(result && !result->unresolved.empty());
        assert(result->operation == "uninterpreted_call" ||
               contains(result->unresolved, "call_namespace_not_resolved"));
    }

    const auto unknown = prepare(
        R"({"type":"function_call","name":"other_tool","namespace":"tools","arguments":"{}"})");
    assert(unknown && unknown->name_space == "tools");
    assert(unknown->unresolved == std::vector<std::string>({"tool_semantics_not_resolved"}));

    const auto qualified = prepare(
        R"({"type":"custom_tool_call","name":"exec_command","namespace":"functions","condition":"unknown","input":{"cmd":"pytest"}})");
    assert(qualified && contains(qualified->unresolved,
                                 "call_event_qualifications_not_resolved"));
}

void test_missing_or_unstructured_fields_stay_explicit() {
    const auto missing = prepare(
        R"({"type":"custom_tool_call","name":"exec_command"})");
    assert(missing && missing->operation == "uninterpreted_call");
    assert(contains(missing->unresolved, "call_event_qualifications_not_resolved"));
    assert(contains(missing->unresolved, "call_arguments_not_structured"));

    const auto bad_name = prepare(
        R"({"type":"custom_tool_call","name":false,"input":{"cmd":"pytest"}})");
    assert(bad_name && !bad_name->tool_name);
    assert(contains(bad_name->unresolved, "tool_semantics_not_resolved"));
}

}  // namespace

int main() {
    test_literal_request_is_parsed_without_execution_or_effect_claims();
    test_shell_programs_are_not_reported_as_one_literal_command();
    test_options_are_typed_and_invalid_values_remain_unresolved();
    test_function_arguments_duplicate_keys_and_nonfinite_numbers();
    test_python_json_number_spelling_for_source_hashes();
    test_python_string_repr_spelling();
    test_namespace_is_exact_recorded_identity();
    test_missing_or_unstructured_fields_stay_explicit();
    assert(world::session_call_content_source_sha256 ==
           "06ee0cdf201c863af7a2e32b621357c7e4b0f0c26ce03de40d2209470b5f1f16");
    std::cout << "PASS session call cold literal interpretation and authority boundary\n";
}
