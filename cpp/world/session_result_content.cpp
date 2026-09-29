#include "world/session_result_content.hpp"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <memory_resource>
#include <stdexcept>
#include <utility>

namespace swegca::world {
namespace {

[[nodiscard]] bool string_value(const transport::Json* value,
                                std::string_view& result) noexcept {
    if (!value || value->kind != transport::Json::Kind::string) return false;
    result = value->scalar;
    return true;
}

[[nodiscard]] bool decimal_magnitude_less_than_one(
    const std::string_view token) noexcept {
    constexpr std::int64_t limit = 1LL << 50;
    auto index = std::size_t{token.starts_with('-') ? 1U : 0U};
    std::int64_t digits_before_decimal = 0;
    std::int64_t digit_index = 0;
    std::int64_t first_nonzero = -1;
    bool before_decimal = true;
    for (; index < token.size() && token[index] != 'e' && token[index] != 'E';
         ++index) {
        if (token[index] == '.') {
            before_decimal = false;
            continue;
        }
        if (before_decimal && digits_before_decimal < limit)
            ++digits_before_decimal;
        if (first_nonzero < 0 && token[index] != '0')
            first_nonzero = digit_index;
        if (digit_index < limit) ++digit_index;
    }
    if (first_nonzero < 0) return true;

    std::int64_t exponent = 0;
    if (index < token.size()) {
        ++index;
        bool negative = false;
        if (index < token.size() && (token[index] == '+' || token[index] == '-')) {
            negative = token[index] == '-';
            ++index;
        }
        for (; index < token.size(); ++index) {
            const auto digit = static_cast<std::int64_t>(token[index] - '0');
            if (exponent > (limit - digit) / 10) {
                exponent = limit;
                break;
            }
            exponent = exponent * 10 + digit;
        }
        if (negative) exponent = -exponent;
    }
    return exponent + digits_before_decimal - first_nonzero - 1 < 0;
}

[[nodiscard]] bool finite_numbers(const transport::Json& value) {
    if (value.kind == transport::Json::Kind::number &&
        value.scalar.find_first_of(".eE") != std::string_view::npos) {
        double parsed = 0.0;
        const auto begin = value.scalar.data();
        const auto converted = std::from_chars(
            begin, begin + value.scalar.size(), parsed, std::chars_format::general);
        if (converted.ptr != begin + value.scalar.size() ||
            (converted.ec != std::errc{} &&
             !(converted.ec == std::errc::result_out_of_range &&
               decimal_magnitude_less_than_one(value.scalar))) ||
            (converted.ec == std::errc{} && !std::isfinite(parsed)))
            return false;
    }
    for (const auto& child : value.values)
        if (!finite_numbers(child)) return false;
    return true;
}

[[nodiscard]] bool integer_token(const transport::Json* value) noexcept {
    return value && value->kind == transport::Json::Kind::number &&
        value->scalar.find_first_of(".eE") == std::string_view::npos;
}

[[nodiscard]] SessionDecimalInteger decimal_integer(
    const std::string_view token) {
    return SessionDecimalInteger{token == "-0" ? "0" : std::string(token)};
}

[[nodiscard]] bool positive_integer(
    const SessionDecimalInteger& value) noexcept {
    return value.value != "0" && !value.value.starts_with('-');
}

[[nodiscard]] bool empty_string(const transport::Json& value) noexcept {
    return value.kind == transport::Json::Kind::string && value.scalar.empty();
}

}  // namespace

SessionResultMeaning::SessionResultMeaning(
    std::optional<SessionDecimalInteger> exit_code_value,
    std::optional<bool> tool_reported_error_value,
    std::optional<SessionDecimalInteger> process_session_id_value,
    std::string interpretation_value,
    std::vector<std::string> unresolved_value)
    : exit_code(std::move(exit_code_value)),
      tool_reported_error(std::move(tool_reported_error_value)),
      process_session_id(std::move(process_session_id_value)),
      interpretation(std::move(interpretation_value)),
      unresolved(std::move(unresolved_value)) {}

std::optional<SessionResultMeaning> prepare_result_meaning(
    const transport::Json& payload) {
    if (payload.kind != transport::Json::Kind::object) return std::nullopt;
    std::string_view event_type;
    if (!string_value(payload.find("type"), event_type) ||
        (event_type != "function_call_output" &&
         event_type != "custom_tool_call_output"))
        return std::nullopt;

    const transport::Json* value = payload.find("output");
    std::pmr::monotonic_buffer_resource nested_memory;
    std::optional<transport::Json> parsed_output;
    if (value && value->kind == transport::Json::Kind::string) {
        try {
            parsed_output.emplace(
                transport::parse_json(value->scalar, nested_memory, 1000));
            if (!finite_numbers(*parsed_output)) parsed_output.reset();
        } catch (const std::exception&) {
            parsed_output.reset();
        }
        if (!parsed_output)
            return SessionResultMeaning(std::nullopt, std::nullopt, std::nullopt,
                "uninterpreted_result", {"unstructured_result_text"});
        value = &*parsed_output;
    }
    if (!value || value->kind != transport::Json::Kind::object)
        return SessionResultMeaning(std::nullopt, std::nullopt, std::nullopt,
            "uninterpreted_result", {"unknown_result_shape"});

    std::vector<std::string> unresolved;
    if (payload.keys.size() != 2 || !payload.find("type") || !payload.find("output"))
        unresolved.emplace_back("event_qualifications_not_resolved");

    std::optional<SessionDecimalInteger> exit_code;
    if (const auto* exit = value->find("exit_code"); exit &&
        exit->kind != transport::Json::Kind::null) {
        if (integer_token(exit))
            exit_code = decimal_integer(exit->scalar);
        else
            unresolved.emplace_back("invalid_exit_code_type");
    }

    std::optional<bool> error;
    if (const auto* reported = value->find("isError"); reported &&
        reported->kind != transport::Json::Kind::null) {
        if (reported->kind == transport::Json::Kind::boolean)
            error = reported->scalar == "true";
        else
            unresolved.emplace_back("invalid_tool_error_type");
    }

    std::optional<SessionDecimalInteger> session_id;
    if (const auto* session = value->find("session_id"); session &&
        session->kind != transport::Json::Kind::null) {
        if (integer_token(session)) {
            auto candidate = decimal_integer(session->scalar);
            if (positive_integer(candidate))
                session_id = std::move(candidate);
            else
                unresolved.emplace_back("invalid_process_session_type");
        } else {
            unresolved.emplace_back("invalid_process_session_type");
        }
    }

    for (std::size_t index = 0; index < value->keys.size(); ++index) {
        const auto key = std::string_view(value->keys[index]);
        if (key == "exit_code" || key == "isError" || key == "session_id")
            continue;
        if ((key == "output" || key == "stdout" || key == "stderr") &&
            empty_string(value->values[index]))
            continue;
        unresolved.emplace_back("result_field_not_resolved:" + std::string(key));
    }

    std::string interpretation;
    if (exit_code && session_id) {
        interpretation = "ambiguous_process_state";
        unresolved.emplace_back("termination_and_running_handle_both_present");
    } else if (error == true) {
        interpretation = "tool_reported_error";
    } else if (exit_code) {
        interpretation = exit_code->value == "0" ?
            "recorded_normal_exit" : "recorded_nonzero_exit";
    } else if (session_id) {
        interpretation = "recorded_running_handle_not_completion";
    } else {
        interpretation = "uninterpreted_result";
        unresolved.emplace_back("no_process_completion_evidence");
    }
    return SessionResultMeaning(std::move(exit_code), std::move(error),
        std::move(session_id), std::move(interpretation), std::move(unresolved));
}

std::string session_result_text(
    const SessionResultMeaning& meaning,
    const std::span<const SessionCallMeaning> calls) {
    std::string text;
    if (meaning.interpretation == "recorded_normal_exit") {
        text = "과거 도구 반환에 종료 코드 0이 기록되어 있다. 프로세스의 정상 종료 기록이며 작업의 실제 성공 증명은 아니다.";
    } else if (meaning.interpretation == "recorded_nonzero_exit") {
        text = "과거 도구 반환의 종료 코드는 " +
            (meaning.exit_code ? meaning.exit_code->value : "None") +
            "이며 비정상 종료가 기록되어 있다.";
    } else if (meaning.interpretation == "tool_reported_error") {
        text = "과거 도구 반환은 도구 오류를 명시적으로 보고했다.";
    } else if (meaning.interpretation == "recorded_running_handle_not_completion") {
        text = "과거 도구 반환은 실행 중인 프로세스 핸들을 돌려줬다. 그 반환만으로 완료 여부를 알 수 없다.";
    } else if (meaning.interpretation == "ambiguous_process_state") {
        text = "과거 도구 반환에 종료 코드와 실행 핸들이 함께 있어 상태 해석이 미해결이다.";
    } else {
        text = "이 도구 반환의 실행 결과 내용은 아직 해석되지 않았다.";
    }
    if (!meaning.unresolved.empty())
        text += " 부가 출력이나 조건의 의미 처리가 남아 있으므로 이 부분만으로 전체 답이 완성되지는 않는다.";
    if (!calls.empty()) {
        std::string requests;
        for (std::size_t index = 0; index < calls.size(); ++index) {
            if (index) requests.push_back('\n');
            requests += call_request_text(calls[index]);
        }
        text = requests + "\n동일 session/turn/call ID로 연결된 반환 기록: " + text;
    }
    return text;
}

}  // namespace swegca::world
