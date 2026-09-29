#include "world/session_call_content.hpp"

#include "world/unicode_nfkc.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <memory_resource>
#include <set>
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
    const auto order = exponent + digits_before_decimal - first_nonzero - 1;
    return order < 0;
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

[[nodiscard]] bool integer_token(const transport::Json& value) noexcept {
    return value.kind == transport::Json::Kind::number &&
        value.scalar.find_first_of(".eE") == std::string_view::npos;
}

[[nodiscard]] bool integer_nonnegative(const std::string_view token) noexcept {
    if (token.empty() || token.front() != '-') return !token.empty();
    return std::ranges::all_of(token.substr(1), [](const char value) { return value == '0'; });
}

[[nodiscard]] std::string normalized_integer(const std::string_view token) {
    if (token == "-0") return "0";
    return std::string(token);
}

[[nodiscard]] std::optional<std::vector<std::string>> split_simple_command(
    const std::string_view command) {
    std::vector<std::string> result;
    std::string current;
    bool token_started = false;
    enum class Quote { none, single, double_quote } quote = Quote::none;
    for (std::size_t index = 0; index < command.size(); ++index) {
        const auto value = command[index];
        if (quote == Quote::none) {
            if (value == ' ' || value == '\t') {
                if (token_started) {
                    result.push_back(std::move(current));
                    current.clear();
                    token_started = false;
                }
            } else if (value == '\'') {
                quote = Quote::single;
                token_started = true;
            } else if (value == '"') {
                quote = Quote::double_quote;
                token_started = true;
            } else if (value == '\\') {
                if (++index >= command.size()) return std::nullopt;
                current.push_back(command[index]);
                token_started = true;
            } else {
                current.push_back(value);
                token_started = true;
            }
        } else if (quote == Quote::single) {
            if (value == '\'') quote = Quote::none;
            else current.push_back(value);
        } else {
            if (value == '"') {
                quote = Quote::none;
            } else if (value == '\\') {
                if (++index >= command.size()) return std::nullopt;
                const auto escaped = command[index];
                if (escaped == '"' || escaped == '\\') current.push_back(escaped);
                else {
                    current.push_back('\\');
                    current.push_back(escaped);
                }
            } else {
                current.push_back(value);
            }
        }
    }
    if (quote != Quote::none) return std::nullopt;
    if (token_started) result.push_back(std::move(current));
    return result;
}

void append_unique(std::vector<std::string>& values, std::string value) {
    if (std::ranges::find(values, value) == values.end())
        values.push_back(std::move(value));
}

[[nodiscard]] std::string python_repr(const std::string_view value) {
    std::string result{"'"};
    for (const auto byte : value) {
        switch (byte) {
        case '\\': result += "\\\\"; break;
        case '\'': result += "\\'"; break;
        case '\t': result += "\\t"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        default: result.push_back(byte);
        }
    }
    result.push_back('\'');
    return result;
}

[[nodiscard]] std::string tuple_repr(const std::vector<std::string>& values) {
    std::string result{"("};
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index) result += ", ";
        result += python_repr(values[index]);
    }
    if (values.size() == 1) result.push_back(',');
    result.push_back(')');
    return result;
}

[[nodiscard]] std::string option_value_repr(const SessionCallOptionValue& value) {
    if (const auto* text = std::get_if<std::string>(&value)) return python_repr(*text);
    if (const auto* boolean = std::get_if<bool>(&value)) return *boolean ? "True" : "False";
    return std::get<SessionDecimalInteger>(value).value;
}

[[nodiscard]] std::string options_repr(const std::vector<SessionCallOption>& values) {
    std::string result{"("};
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index) result += ", ";
        result += "(" + python_repr(values[index].name) + ", " +
            option_value_repr(values[index].value) + ")";
    }
    if (values.size() == 1) result.push_back(',');
    result.push_back(')');
    return result;
}

}  // namespace

SessionCallMeaning::SessionCallMeaning(
    std::optional<std::string> tool_name_value,
    std::string operation_value,
    std::optional<std::string> executable_value,
    std::vector<std::string> arguments_value,
    std::vector<SessionCallOption> requested_options_value,
    std::vector<std::string> unresolved_value,
    std::optional<std::string> namespace_value)
    : tool_name(std::move(tool_name_value)), operation(std::move(operation_value)),
      executable(std::move(executable_value)), arguments(std::move(arguments_value)),
      requested_options(std::move(requested_options_value)),
      unresolved(std::move(unresolved_value)), name_space(std::move(namespace_value)) {}

bool SessionCallMeaning::addresses_subject(const std::string_view subject) const {
    if (!unresolved.empty()) return false;
    const auto folded = unicode_casefold(subject);
    return (tool_name && unicode_casefold(*tool_name) == folded) ||
        (executable && unicode_casefold(*executable) == folded);
}

std::optional<SessionCallMeaning> prepare_call_meaning(const transport::Json& payload) {
    if (payload.kind != transport::Json::Kind::object) return std::nullopt;
    std::string_view kind;
    if (!string_value(payload.find("type"), kind) ||
        (kind != "custom_tool_call" && kind != "function_call"))
        return std::nullopt;

    const auto argument_field = kind == "custom_tool_call" ? "input" : "arguments";
    std::vector<std::string> unresolved;
    if (!payload.find("type") || !payload.find("name") || !payload.find(argument_field))
        unresolved.emplace_back("call_event_qualifications_not_resolved");
    for (const auto& key : payload.keys)
        if (key != "type" && key != "name" && key != argument_field && key != "namespace") {
            if (std::ranges::find(unresolved, "call_event_qualifications_not_resolved") ==
                unresolved.end())
                unresolved.emplace_back("call_event_qualifications_not_resolved");
            break;
        }

    std::optional<std::string> name_space;
    if (const auto* value = payload.find("namespace")) {
        std::string_view text;
        if (string_value(value, text) && !strip_unicode_whitespace(text).empty())
            name_space.emplace(text);
        else
            unresolved.emplace_back("call_namespace_not_resolved");
    }

    std::optional<std::string> name;
    std::string_view name_value;
    if (string_value(payload.find("name"), name_value) && !name_value.empty())
        name.emplace(name_value);

    const transport::Json* arguments = payload.find(argument_field);
    std::pmr::monotonic_buffer_resource nested_memory;
    std::optional<transport::Json> parsed_arguments;
    if (arguments && arguments->kind == transport::Json::Kind::string) {
        try {
            parsed_arguments.emplace(transport::parse_json(arguments->scalar, nested_memory, 1000));
            if (!finite_numbers(*parsed_arguments)) parsed_arguments.reset();
        } catch (const std::exception&) {
            parsed_arguments.reset();
        }
        arguments = parsed_arguments ? &*parsed_arguments : nullptr;
    }
    if (!arguments || arguments->kind != transport::Json::Kind::object) {
        unresolved.emplace_back("call_arguments_not_structured");
        return SessionCallMeaning(std::move(name), "uninterpreted_call", std::nullopt,
            {}, {}, std::move(unresolved), std::move(name_space));
    }

    const auto supported_name = name &&
        (*name == "exec_command" || *name == "functions.exec_command");
    const auto supported_namespace = !name_space || *name_space == "functions";
    if (!supported_name || !supported_namespace) {
        unresolved.emplace_back("tool_semantics_not_resolved");
        return SessionCallMeaning(std::move(name), "uninterpreted_call", std::nullopt,
            {}, {}, std::move(unresolved), std::move(name_space));
    }

    std::vector<std::string> argv;
    std::string_view command;
    static constexpr char forbidden_characters[] =
        "\n\r\0;&|<>()$`*?[]{}~#";
    static constexpr std::string_view forbidden{
        forbidden_characters, sizeof(forbidden_characters) - 1};
    if (!string_value(arguments->find("cmd"), command) ||
        strip_unicode_whitespace(command).empty() ||
        command.find_first_of(forbidden) != std::string_view::npos) {
        unresolved.emplace_back("shell_program_semantics_not_resolved");
    } else if (auto parsed = split_simple_command(command)) {
        argv = std::move(*parsed);
    } else {
        unresolved.emplace_back("shell_program_semantics_not_resolved");
    }

    static const std::set<std::string_view> reserved{
        "if", "then", "elif", "else", "fi", "for", "while", "until",
        "do", "done", "case", "esac", "in", "!", "time", "function"};
    if (!argv.empty() && (argv.front().find('=') != std::string::npos ||
                         reserved.contains(argv.front()))) {
        argv.clear();
        unresolved.emplace_back("shell_program_semantics_not_resolved");
    }
    if (argv.empty() || argv.front().empty())
        unresolved.emplace_back("no_literal_command_target");

    std::vector<SessionCallOption> options;
    for (std::size_t index = 0; index < arguments->keys.size(); ++index) {
        const auto key = std::string_view(arguments->keys[index]);
        const auto& child = arguments->values[index];
        if (key == "cmd") continue;
        bool valid = false;
        SessionCallOptionValue option_value{std::string{}};
        if ((key == "workdir" || key == "shell") &&
            child.kind == transport::Json::Kind::string && !child.scalar.empty()) {
            valid = true;
            option_value = std::string(child.scalar);
        } else if ((key == "login" || key == "tty") &&
                   child.kind == transport::Json::Kind::boolean) {
            valid = true;
            option_value = child.scalar == "true";
        } else if ((key == "yield-time-ms" || key == "max_output_tokens") &&
                   integer_token(child) && integer_nonnegative(child.scalar)) {
            valid = true;
            option_value = SessionDecimalInteger{normalized_integer(child.scalar)};
        }
        if (valid)
            options.push_back({std::string(key), std::move(option_value)});
        else
            unresolved.emplace_back("call_option_not_resolved:" + std::string(key));
    }
    if (const auto* shell = arguments->find("shell"); shell &&
        !(shell->kind == transport::Json::Kind::null)) {
        std::string_view shell_value;
        if (!string_value(shell, shell_value) ||
            (shell_value != "sh" && shell_value != "bash" &&
             shell_value != "/bin/sh" && shell_value != "/bin/bash" &&
             shell_value != "/usr/bin/bash"))
            unresolved.emplace_back("requested_shell_semantics_not_resolved");
    }

    std::vector<std::string> arguments_out;
    std::optional<std::string> executable;
    if (!argv.empty()) {
        executable = argv.front();
        arguments_out.assign(argv.begin() + 1, argv.end());
    }
    std::vector<std::string> unique_unresolved;
    for (auto& value : unresolved) append_unique(unique_unresolved, std::move(value));
    return SessionCallMeaning(
        std::move(name), argv.empty() ? "uninterpreted_call" : "requested_simple_command",
        std::move(executable), std::move(arguments_out), std::move(options),
        std::move(unique_unresolved), std::move(name_space));
}

std::string call_request_text(const SessionCallMeaning& meaning) {
    if (meaning.operation != "requested_simple_command")
        return "과거 호출 요청의 내용 해석이 아직 미완료다.";
    auto text = meaning.name_space
        ? "기록된 namespace는 " + python_repr(*meaning.name_space) + "이다. "
        : std::string{};
    return text + "과거 " +
        (meaning.tool_name ? python_repr(*meaning.tool_name) : "None") +
        " 호출은 셸을 통해 명령 대상 " +
        (meaning.executable ? python_repr(*meaning.executable) : "None") +
        "에 인수 " + tuple_repr(meaning.arguments) +
        "를 전달하도록 요청했다. 명시된 실행 옵션은 " +
        options_repr(meaning.requested_options) +
        "이다. 이는 요청 내용이며 실제 대상 실행이나 프로그램의 효과가 확인됐다는 뜻은 아니다.";
}

}  // namespace swegca::world
