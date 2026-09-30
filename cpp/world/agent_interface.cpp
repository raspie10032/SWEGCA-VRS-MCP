#include "world/agent_interface.hpp"

#include "world/expression_wire.hpp"
#include "world/provider_cancellation.hpp"
#include "world/semantic_vrs_ingress.hpp"
#include "world/utterance_receipt.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <limits>
#include <poll.h>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace swegca::world {
namespace {

const JsonValue* find(const JsonValue::Object& object, const std::string_view key) {
    const auto item = object.find(key);
    return item == object.end() ? nullptr : &item->second;
}

const std::string* string(const JsonValue* value) {
    return value ? std::get_if<std::string>(&value->storage()) : nullptr;
}

const bool* boolean(const JsonValue* value) {
    return value ? std::get_if<bool>(&value->storage()) : nullptr;
}

const std::int64_t* integer(const JsonValue* value) {
    return value ? std::get_if<std::int64_t>(&value->storage()) : nullptr;
}

bool has_text(const std::string_view value) {
    return std::ranges::any_of(value, [](const unsigned char byte) {
        return byte != ' ' && byte != '\t' && byte != '\n' && byte != '\r' &&
               byte != '\f' && byte != '\v';
    });
}

std::string checked_text(std::string value, const std::size_t maximum = 65'536) {
    if (!has_text(value) || value.size() > maximum) throw InterfaceError("invalid_text");
    return value;
}

bool truthy(const JsonValue* value) {
    if (!value || std::holds_alternative<std::nullptr_t>(value->storage())) return false;
    if (const auto* flag = boolean(value)) return *flag;
    if (const auto* number = integer(value)) return *number != 0;
    if (const auto* number = std::get_if<double>(&value->storage())) return *number != 0.0;
    if (const auto* value_string = string(value)) return !value_string->empty();
    if (value->is_array()) return !value->as_array().empty();
    return !value->as_object().empty();
}

std::int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool has_reference(const JsonValue& value) {
    if (value.is_object()) {
        if (value.as_object().contains("$ref")) return true;
        return std::ranges::any_of(value.as_object(), [](const auto& row) {
            return has_reference(row.second);
        });
    }
    return value.is_array() && std::ranges::any_of(value.as_array(), has_reference);
}

bool safe_provider_failure(const std::string_view code) {
    if (code == "provider_request_failed" ||
        code == "invalid_or_nontext_provider_proposal" ||
        code == "payload_too_large") return true;
    if (!code.starts_with("provider_http_") || code.size() != 17) return false;
    unsigned status{};
    const auto parsed = std::from_chars(
        code.data() + 14, code.data() + code.size(), status);
    return parsed.ec == std::errc{} && status >= 400 && status <= 599;
}

JsonValue optional_text(const std::optional<std::string>& value) {
    return value ? JsonValue(*value) : JsonValue(nullptr);
}

std::string profile_text(const JsonValue::Object& object, const std::string_view key) {
    const auto* result = string(find(object, key));
    if (!result) throw InterfaceError("invalid_profile");
    return *result;
}

void write_all(const int file, const std::string_view data) {
    std::size_t offset{};
    while (offset < data.size()) {
        const auto count = ::write(file, data.data() + offset, data.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) throw InterfaceError("resident_request_failed");
        offset += static_cast<std::size_t>(count);
    }
}

JsonValue::Object with_arguments(
    const std::string_view command, JsonValue::Object arguments) {
    arguments.emplace("command", std::string(command));
    return arguments;
}

bool exact_keys(const JsonValue::Object& object,
                const std::set<std::string, std::less<>>& required,
                const std::set<std::string, std::less<>>& optional) {
    if (std::ranges::any_of(required, [&](const auto& key) {
            return !object.contains(key);
        })) return false;
    return std::ranges::all_of(object, [&](const auto& row) {
        return required.contains(row.first) || optional.contains(row.first);
    });
}

const std::string& required_string(
    const JsonValue::Object& object, const std::string_view key,
    const char* error = "resident_operation_not_exported") {
    const auto* value = string(find(object, key));
    if (!value) throw InterfaceError(error);
    return *value;
}

std::filesystem::path temporary_directory() {
    auto pattern = (std::filesystem::temp_directory_path() /
                    "rozephine-codex-worker-XXXXXX").string();
    std::vector<char> bytes(pattern.begin(), pattern.end());
    bytes.push_back('\0');
    if (!::mkdtemp(bytes.data())) throw InterfaceError("provider_request_failed");
    return std::filesystem::path(bytes.data());
}

}  // namespace

ChatAdapter::ChatAdapter(ProviderProfile profile, ProviderPost post)
    : profile_(std::move(profile)), post_(std::move(post)) {}

std::string ChatAdapter::generate(const DetachedProposalRequest& request) {
    auto parameters = provider_decode_object(profile_.parameters_utf8);
    auto evidence = provider_decode_object(request.current_evidence_json_utf8);
    const auto* schema = string(find(evidence, "schema"));
    const auto* complete = boolean(find(evidence, "main_cognition_complete"));
    const auto* scope = string(find(evidence, "source_scope"));
    const bool final_expression = schema && *schema == "rozephine-final-utterance-v2" &&
        complete && *complete && scope &&
        *scope == "completed_main_answer_wording_only";
    if (final_expression && request.current_evidence_json_utf8.size() > 4096) {
        const auto* part = find(evidence, "utterance_part");
        bool plain_part{};
        if (part && part->is_object()) {
            const auto* part_schema = string(find(part->as_object(), "schema"));
            const auto* state = string(find(part->as_object(), "state"));
            plain_part = part_schema && *part_schema == "rozephine-utterance-receipt-v1" &&
                         state && *state == "prepared";
        }
        auto compact = compact_expression(JsonValue(evidence), plain_part);
        if (semantic_canonical_json(compact).size() <
            semantic_canonical_json(JsonValue(evidence)).size())
            evidence = compact.as_object();
    }
    JsonValue::Object envelope{
        {"instruction", request.prompt()}, {"current_evidence", evidence},
        {"snapshot_id", request.full_current_pair_snapshot_id},
        {"source_revision", request.source_revision}};
    auto content = semantic_canonical_json(JsonValue(std::move(envelope)));
    if (content.size() > provider_max_bytes) throw InterfaceError("payload_too_large");
    const std::string message =
        "Return a text proposal only. You have no tool, action, memory-write or semantic authority.\n" +
        content;
    JsonValue::Object body{
        {"model", profile_.model},
        {"messages", JsonValue::Array{JsonValue::Object{
            {"role", "user"}, {"content", message}}}}, {"stream", false}};
    if (profile_.protocol == "chat_completions") {
        static const std::set<std::string, std::less<>> allowed{
            "temperature", "top_p", "max_tokens", "max_completion_tokens",
            "reasoning_effort", "seed", "stop", "response_format"};
        if (std::ranges::any_of(parameters, [&](const auto& row) {
                return !allowed.contains(row.first);
            })) throw InterfaceError("unsupported_generation_parameter");
        if (const auto* format = find(parameters, "response_format")) {
            if (!format->is_object()) throw InterfaceError("invalid_response_format");
            const auto& object = format->as_object();
            const auto* type = string(find(object, "type"));
            if (!type || *type != "json_object" || object.size() > 2 ||
                std::ranges::any_of(object, [](const auto& row) {
                    return row.first != "type" && row.first != "schema";
                }) || semantic_canonical_json(*format).size() > 16'384 ||
                (find(object, "schema") && !find(object, "schema")->is_object()))
                throw InterfaceError("invalid_response_format");
            if (has_reference(*format))
                throw InterfaceError("response_schema_references_not_allowed");
        }
        for (auto& [key, value] : parameters) body.insert_or_assign(key, std::move(value));
        if (!body.contains("max_tokens") && !body.contains("max_completion_tokens"))
            body.emplace("max_tokens", 256);
    } else if (profile_.protocol == "ollama_chat") {
        static const std::set<std::string, std::less<>> allowed{
            "temperature", "top_p", "num_ctx", "num_predict", "seed", "stop"};
        if (std::ranges::any_of(parameters, [&](const auto& row) {
                return !allowed.contains(row.first);
            })) throw InterfaceError("unsupported_generation_parameter");
        JsonValue::Object options{{"num_predict", 256}};
        for (auto& [key, value] : parameters) options.insert_or_assign(key, std::move(value));
        body.emplace("options", std::move(options));
    } else throw InterfaceError("unsupported_protocol");
    for (const auto name : {"max_tokens", "max_completion_tokens", "num_predict", "num_ctx"}) {
        if (const auto* value = find(parameters, name)) {
            const auto* budget = integer(value);
            if (!budget || *budget <= 0 || *budget > 131'072)
                throw InterfaceError("invalid_token_budget");
        }
    }
    const auto reply = post_(profile_, body);
    try {
        const JsonValue::Object* response_message{};
        if (profile_.protocol == "chat_completions") {
            const auto* choices = find(reply, "choices");
            if (!choices || !choices->is_array() || choices->as_array().size() != 1 ||
                !choices->as_array().front().is_object()) throw std::invalid_argument("reply");
            const auto& choice = choices->as_array().front().as_object();
            const auto* reason = string(find(choice, "finish_reason"));
            const auto* message_value = find(choice, "message");
            if (!reason || *reason != "stop" || !message_value || !message_value->is_object())
                throw std::invalid_argument("reply");
            response_message = &message_value->as_object();
        } else {
            const auto* done = boolean(find(reply, "done"));
            const auto* reason = string(find(reply, "done_reason"));
            const auto* message_value = find(reply, "message");
            if (!done || !*done || (reason && *reason != "stop") ||
                !message_value || !message_value->is_object())
                throw std::invalid_argument("reply");
            response_message = &message_value->as_object();
        }
        if (truthy(find(*response_message, "tool_calls")) ||
            truthy(find(*response_message, "function_call")) ||
            truthy(find(*response_message, "refusal"))) throw std::invalid_argument("reply");
        const auto* proposal = string(find(*response_message, "content"));
        if (!proposal) throw std::invalid_argument("reply");
        return checked_text(*proposal);
    } catch (const std::exception&) {
        throw InterfaceError("invalid_or_nontext_provider_proposal");
    }
}

std::string run_codex_bounded(
    const std::vector<std::string>& command, const std::string_view prompt,
    const double timeout_seconds) {
    check_cancelled();
    if (command.empty() || !std::isfinite(timeout_seconds) || timeout_seconds <= 0)
        throw InterfaceError("provider_request_failed");
    int input[2]{-1, -1};
    int output[2]{-1, -1};
    int error[2]{-1, -1};
    if (::pipe(input) || ::pipe(output) || ::pipe(error))
        throw InterfaceError("provider_request_failed");
    const pid_t child = ::fork();
    if (child < 0) throw InterfaceError("provider_request_failed");
    if (child == 0) {
        (void)::setpgid(0, 0);
        (void)::dup2(input[0], STDIN_FILENO);
        (void)::dup2(output[1], STDOUT_FILENO);
        (void)::dup2(error[1], STDERR_FILENO);
        for (const int file : {input[0], input[1], output[0], output[1], error[0], error[1]})
            (void)::close(file);
        std::vector<char*> arguments;
        for (const auto& value : command) arguments.push_back(const_cast<char*>(value.c_str()));
        arguments.push_back(nullptr);
        ::execv(arguments.front(), arguments.data());
        ::_exit(127);
    }
    (void)::setpgid(child, child);
    ::close(input[0]); ::close(output[1]); ::close(error[1]);
    const auto cleanup = [&] {
        (void)::kill(-child, SIGKILL);
        int status{};
        while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {}
        for (const int file : {input[1], output[0], error[0]}) (void)::close(file);
    };
    try {
        for (const int file : {input[1], output[0], error[0]}) {
            const int flags = ::fcntl(file, F_GETFL, 0);
            if (flags < 0 || ::fcntl(file, F_SETFL, flags | O_NONBLOCK) < 0)
                throw InterfaceError("provider_request_failed");
        }
        std::string captured;
        std::size_t total{};
        std::size_t sent{};
        bool input_open{true};
        bool output_open{true};
        bool error_open{true};
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::duration<double>(timeout_seconds);
        while (input_open || output_open || error_open) {
            check_cancelled();
            if (std::chrono::steady_clock::now() >= deadline)
                throw InterfaceError("codex_timeout");
            std::array<pollfd, 3> waits{{
                {input[1], static_cast<short>(input_open ? POLLOUT : 0), 0},
                {output[0], static_cast<short>(output_open ? POLLIN : 0), 0},
                {error[0], static_cast<short>(error_open ? POLLIN : 0), 0}}};
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count();
            if (::poll(waits.data(), waits.size(), static_cast<int>(std::min<std::int64_t>(100, remaining))) < 0 &&
                errno != EINTR) throw InterfaceError("provider_request_failed");
            if (input_open && waits[0].revents) {
                const auto count = ::write(input[1], prompt.data() + sent,
                    std::min<std::size_t>(8192, prompt.size() - sent));
                if (count > 0) sent += static_cast<std::size_t>(count);
                else if (errno != EAGAIN && errno != EINTR && errno != EPIPE)
                    throw InterfaceError("provider_request_failed");
                if (sent == prompt.size() || (count < 0 && errno == EPIPE)) {
                    ::close(input[1]); input_open = false;
                }
            }
            for (std::size_t index = 1; index < waits.size(); ++index) {
                bool& open = index == 1 ? output_open : error_open;
                if (!open || !waits[index].revents) continue;
                std::array<char, 8192> buffer{};
                const auto count = ::read(waits[index].fd, buffer.data(), buffer.size());
                if (count > 0) {
                    total += static_cast<std::size_t>(count);
                    if (total > provider_max_bytes)
                        throw InterfaceError("codex_output_too_large");
                    if (index == 1) captured.append(buffer.data(), static_cast<std::size_t>(count));
                } else if (!count || (errno != EAGAIN && errno != EINTR)) {
                    ::close(waits[index].fd); open = false;
                }
            }
        }
        int status{};
        while (::waitpid(child, &status, WNOHANG) == 0) {
            check_cancelled();
            if (std::chrono::steady_clock::now() >= deadline)
                throw InterfaceError("codex_timeout");
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
            throw InterfaceError("codex_process_failed");
        (void)::kill(-child, SIGKILL);
        for (const int file : {input[1], output[0], error[0]}) (void)::close(file);
        return captured;
    } catch (...) {
        cleanup();
        throw;
    }
}

CodexExecAdapter::CodexExecAdapter(ProviderProfile profile, CodexRun run)
    : profile_(std::move(profile)), run_(std::move(run)) {}

std::string CodexExecAdapter::generate(const DetachedProposalRequest& request) {
    const auto parameters = provider_decode_object(profile_.parameters_utf8);
    const auto* effort_value = find(parameters, "reasoning_effort");
    const std::string effort = effort_value && string(effort_value)
        ? *string(effort_value) : "low";
    const auto payload = provider_encode(JsonValue::Object{
        {"instruction", checked_text(request.prompt())},
        {"current_evidence", provider_decode_object(request.current_evidence_json_utf8)},
        {"snapshot_id", request.full_current_pair_snapshot_id},
        {"source_revision", request.source_revision}});
    const std::string prompt =
        "You are a replaceable Rozephine text-proposal specialist, not its identity "
        "or final judgment owner. Do not use tools, read files, execute commands, "
        "browse, delegate, write memory, or claim authority. Use only the detached "
        "request below. Return one concise final text proposal, no commentary.\n" + payload;
    if (prompt.size() > provider_max_bytes) throw InterfaceError("payload_too_large");
    const auto scratch = temporary_directory();
    std::string raw;
    try {
        const std::vector<std::string> command{
            profile_.endpoint, "exec", "--ephemeral", "--ignore-user-config",
            "--ignore-rules", "--skip-git-repo-check", "-C", scratch.string(),
            "-s", "read-only", "-m", profile_.model,
            "-c", "model_reasoning_effort=\"" + effort + "\"",
            "-c", "features.shell_tool=false", "-c", "features.apps=false",
            "-c", "features.plugins=false", "-c", "features.multi_agent=false",
            "-c", "web_search=\"disabled\"", "-c", "project_doc_max_bytes=0",
            "--json", "-"};
        raw = run_(command, prompt, profile_.timeout_seconds);
    } catch (...) {
        std::filesystem::remove_all(scratch);
        throw;
    }
    std::filesystem::remove_all(scratch);
    if (raw.size() > provider_max_bytes || raw.empty() || raw.back() != '\n')
        throw InterfaceError("codex_invalid_event_stream");
    std::vector<std::string> messages;
    std::size_t threads{};
    std::size_t turns{};
    std::size_t cursor{};
    while (cursor < raw.size()) {
        const auto end = raw.find('\n', cursor);
        const auto event = provider_decode_object(
            std::string_view(raw).substr(cursor, end - cursor));
        cursor = end + 1;
        const auto* kind = string(find(event, "type"));
        if (!kind) throw InterfaceError("codex_failed_or_unknown_event");
        if (*kind == "thread.started") ++threads;
        else if (*kind == "turn.completed") ++turns;
        else if (*kind == "item.started" || *kind == "item.updated" ||
                 *kind == "item.completed") {
            const auto* item = find(event, "item");
            if (!item || !item->is_object())
                throw InterfaceError("codex_nontext_item_rejected");
            const auto* type = string(find(item->as_object(), "type"));
            if (!type || (*type != "agent_message" && *type != "reasoning"))
                throw InterfaceError("codex_nontext_item_rejected");
            if (*kind == "item.completed" && *type == "agent_message") {
                const auto* message = string(find(item->as_object(), "text"));
                if (!message) throw InterfaceError("invalid_text");
                messages.push_back(checked_text(*message));
            }
        } else if (*kind != "turn.started")
            throw InterfaceError("codex_failed_or_unknown_event");
    }
    if (threads != 1 || turns != 1 || messages.size() != 1)
        throw InterfaceError("codex_incomplete_proposal");
    return messages.front();
}

ProviderRegistry::ProviderRegistry(
    std::vector<ProviderProfile> profiles,
    std::map<std::string, ProposalAdapterFactory, std::less<>> factories)
    : factories_({
          {"chat_completions", [](const ProviderProfile& profile) {
               return std::make_unique<ChatAdapter>(profile); }},
          {"ollama_chat", [](const ProviderProfile& profile) {
               return std::make_unique<ChatAdapter>(profile); }},
          {"codex_exec", [](const ProviderProfile& profile) {
               return std::make_unique<CodexExecAdapter>(profile); }}}) {
    for (auto& [name, factory] : factories_) {
        if (const auto replacement = factories.find(name); replacement != factories.end())
            factory = replacement->second;
    }
    for (auto& [name, factory] : factories)
        factories_.insert_or_assign(std::move(name), std::move(factory));
    for (auto& profile : profiles)
        if (!profiles_.emplace(profile.name, std::move(profile)).second)
            throw InterfaceError("duplicate_profile");
    if (profiles_.size() != profiles.size()) throw InterfaceError("duplicate_profile");
    for (const auto& [name, profile] : profiles_) {
        (void)name;
        if (!factories_.contains(profile.protocol))
            throw InterfaceError("unregistered_protocol");
    }
}

JsonValue::Array ProviderRegistry::describe() const {
    JsonValue::Array result;
    for (const auto& [name, profile] : profiles_) {
        (void)name;
        result.emplace_back(JsonValue::Object{
            {"name", profile.name}, {"protocol", profile.protocol},
            {"model", profile.model}, {"enabled", profile.enabled},
            {"text_only", true}});
    }
    return result;
}

JsonValue::Object ProviderRegistry::propose(
    const std::string_view profile_name,
    const DetachedProposalRequest& request) const {
    const auto found = profiles_.find(profile_name);
    if (found == profiles_.end() || !found->second.enabled)
        throw InterfaceError("profile_unavailable");
    const auto started = now_ns();
    std::optional<std::string> proposal;
    std::optional<std::string> error;
    std::string status;
    try {
        check_cancelled();
        proposal = checked_text(factories_.at(found->second.protocol)(found->second)->generate(request));
        check_cancelled();
        status = "completed_proposal";
    } catch (const std::exception& failure) {
        proposal.reset();
        status = "failed_proposal";
        const auto* typed = dynamic_cast<const InterfaceError*>(&failure);
        error = typed && safe_provider_failure(typed->what())
            ? std::string(typed->what()) : "provider_failure";
    }
    return {{"request", request.receipt()}, {"profile", found->second.name},
            {"model", found->second.model}, {"status", status},
            {"proposal", optional_text(proposal)}, {"error", optional_text(error)},
            {"elapsed_ns", now_ns() - started}, {"proposal_only", true},
            {"authority", proposal_authority_false()},
            {"counts_as_new_experience", 0}, {"growth_claimed", false},
            {"facade_retains_conversation", false}};
}

ProfiledProposalEngine::ProfiledProposalEngine(
    std::shared_ptr<const ProviderRegistry> registry, std::string profile)
    : registry_(std::move(registry)), profile_(std::move(profile)) {
    if (!registry_) throw InterfaceError("profile_unavailable");
}

std::string ProfiledProposalEngine::model_instance_id() const {
    return "configured-provider:" + profile_;
}

std::size_t ProfiledProposalEngine::model_load_count() const { return 0; }

std::vector<std::string> ProfiledProposalEngine::generate_batch(
    const std::vector<DetachedProposalRequest>& requests) {
    std::vector<std::string> result;
    result.reserve(requests.size());
    for (const auto& request : requests) {
        const auto proposal = registry_->propose(profile_, request);
        const auto* status = string(find(proposal, "status"));
        const auto* text_value = string(find(proposal, "proposal"));
        if (!status || *status != "completed_proposal" || !text_value)
            throw InterfaceError("provider_failure");
        result.push_back(*text_value);
    }
    return result;
}

ResidentClient::ResidentClient(std::string socket_path, const double timeout_seconds)
    : socket_path_(std::move(socket_path)), timeout_seconds_(timeout_seconds) {
    if (socket_path_.empty() || socket_path_.front() != '/')
        throw InterfaceError("absolute_resident_socket_required");
    if (!std::isfinite(timeout_seconds_) || timeout_seconds_ <= 0 || timeout_seconds_ > 300)
        throw InterfaceError("invalid_timeout");
}

JsonValue::Object ResidentClient::request(
    const std::string_view command, JsonValue::Object arguments) const {
    static const std::map<std::string, std::set<std::string, std::less<>>, std::less<>> fields{
        {"status", {}}, {"cue", {"cue"}},
        {"cognitive_dialogue", {"query", "profile", "request_id", "expected_pair_snapshot_id"}},
        {"cognitive_dialogue_start", {"query", "profile", "request_id", "expected_pair_snapshot_id"}},
        {"cognitive_dialogue_tick", {}},
        {"cognitive_dialogue_result", {"request_id"}},
        {"cognitive_dialogue_continue", {"request_id"}},
        {"cognitive_dialogue_evidence_open", {"request_id"}},
        {"cognitive_dialogue_advance", {"request_id", "view_id", "step"}},
        {"cognitive_dialogue_model_trace", {"request_id", "view_id", "index"}},
        {"cognitive_dialogue_evidence", {"request_id", "view_id", "operation"}},
        {"cognitive_dialogue_cancel", {"request_id"}},
        {"cognitive_dialogue_release", {"request_id"}}};
    const auto operation = fields.find(command);
    std::set<std::string, std::less<>> optional;
    if (command == "cognitive_dialogue" || command == "cognitive_dialogue_start")
        optional.insert("dialogue_mode");
    if (command == "cognitive_dialogue_result" || command == "cognitive_dialogue_continue" ||
        command == "cognitive_dialogue_evidence_open" || command == "cognitive_dialogue_cancel" ||
        command == "cognitive_dialogue_release") optional.insert("view_id");
    if (command == "cognitive_dialogue_evidence")
        optional = {"reference", "key", "cursor", "offset", "cursor_id"};
    if (operation == fields.end() || !exact_keys(arguments, operation->second, optional))
        throw InterfaceError("resident_operation_not_exported");
    if (const auto* mode = find(arguments, "dialogue_mode")) {
        const auto* value = string(mode);
        if (!value || (*value != "memory_report" && *value != "conversation"))
            throw InterfaceError("invalid_dialogue_mode");
    }
    if (command == "cue") checked_text(required_string(arguments, "cue"), 4096);
    try {
        const int connection = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (connection < 0) throw std::runtime_error("socket");
        struct Guard { int file; ~Guard() { if (file >= 0) ::close(file); } } guard{connection};
        const double timeout = command == "cognitive_dialogue" ||
            command == "cognitive_dialogue_result" ||
            command == "cognitive_dialogue_advance" ||
            command == "cognitive_dialogue_continue" ? 120.0 : timeout_seconds_;
        const timeval duration{static_cast<time_t>(timeout),
            static_cast<suseconds_t>((timeout - std::floor(timeout)) * 1'000'000.0)};
        (void)::setsockopt(connection, SOL_SOCKET, SO_RCVTIMEO, &duration, sizeof(duration));
        (void)::setsockopt(connection, SOL_SOCKET, SO_SNDTIMEO, &duration, sizeof(duration));
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        if (socket_path_.size() >= sizeof(address.sun_path)) throw std::runtime_error("path");
        std::memcpy(address.sun_path, socket_path_.c_str(), socket_path_.size() + 1);
        if (::connect(connection, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0)
            throw std::runtime_error("connect");
        auto frame = provider_encode(JsonValue(with_arguments(command, std::move(arguments))));
        frame.push_back('\n');
        write_all(connection, frame);
        std::string raw;
        std::array<char, 8192> buffer{};
        while (raw.find('\n') == std::string::npos && raw.size() <= provider_max_bytes) {
            const auto count = ::read(connection, buffer.data(), buffer.size());
            if (count <= 0) break;
            raw.append(buffer.data(), static_cast<std::size_t>(count));
        }
        const auto newline = raw.find('\n');
        if (newline == std::string::npos || newline > provider_max_bytes)
            throw InterfaceError("resident_frame_invalid");
        auto result = provider_decode_object(std::string_view(raw).substr(0, newline));
        const auto* status = string(find(result, "status"));
        if (status && *status == "rejected") throw InterfaceError("resident_rejected");
        return result;
    } catch (const std::exception&) {
        throw InterfaceError("resident_request_failed");
    }
}

JsonValue::Array ResidentClient::collect_model_trace(
    std::string request_id, std::string view_id, const std::size_t count,
    Observer trace_sink) const {
    JsonValue::Array receipts;
    std::size_t size{};
    for (std::size_t index = 0; index < count; ++index) {
        auto entry = request("cognitive_dialogue_model_trace", {
            {"request_id", request_id}, {"view_id", view_id},
            {"index", static_cast<std::int64_t>(index)}});
        const auto* entry_request = string(find(entry, "request_id"));
        const auto* entry_view = string(find(entry, "view_id"));
        const auto* entry_index = integer(find(entry, "index"));
        const auto* entry_count = integer(find(entry, "count"));
        if (!entry_request || *entry_request != request_id || !entry_view ||
            *entry_view != view_id || !entry_index || *entry_index != static_cast<std::int64_t>(index) ||
            !entry_count || *entry_count != static_cast<std::int64_t>(count))
            throw InterfaceError("model_trace_binding_changed");
        if (trace_sink) trace_sink(entry);
        else {
            size += provider_encode(JsonValue(entry)).size();
            if (size > provider_max_bytes)
                throw InterfaceError("model_trace_budget_stream_before_releasing_same_request");
            receipts.emplace_back(std::move(entry));
        }
    }
    return receipts;
}

JsonValue::Object ResidentClient::scheduled_dialogue(
    JsonValue::Object arguments, Observer trace_sink, Observer on_part) const {
    const auto started = std::chrono::steady_clock::now();
    const auto request_id = required_string(arguments, "request_id");
    const auto snapshot_id = required_string(arguments, "expected_pair_snapshot_id");
    auto admitted = request("cognitive_dialogue_start", arguments);
    const auto* admitted_status = string(find(admitted, "status"));
    if (!admitted_status || *admitted_status != "queued")
        throw InterfaceError("dialogue_not_queued");
    const auto* view = string(find(admitted, "view_id"));
    if (!view || view->empty() || view->size() > 128)
        throw InterfaceError("dialogue_request_incarnation_unavailable");
    const std::string view_id = *view;
    JsonValue::Object binding{{"request_id", request_id}, {"view_id", view_id}};
    std::optional<JsonValue::Object> result;
    std::size_t delivered_parts{};
    std::optional<std::size_t> part_count;
    while (true) {
        if (!result) result = request("cognitive_dialogue_result", binding);
        if (const auto* result_view = string(find(*result, "view_id"));
            result_view && *result_view != view_id)
            throw InterfaceError("dialogue_request_incarnation_changed");
        const auto* status = string(find(*result, "status"));
        if (status && (*status == "awaiting_model" ||
                       *status == "awaiting_query_organization")) {
            if (std::chrono::steady_clock::now() - started >= std::chrono::seconds(120))
                throw InterfaceError("dialogue_pending_resume_or_release_same_request_id");
            const auto* result_view = string(find(*result, "view_id"));
            const auto* step = integer(find(*result, "step"));
            if (!result_view || !step) throw InterfaceError("resident_request_failed");
            result = request("cognitive_dialogue_advance", {
                {"request_id", request_id}, {"view_id", *result_view}, {"step", *step}});
            const auto* receipt = find(*result, "utterance_delivery");
            if (on_part && receipt && receipt->is_object()) {
                const auto* state = string(find(receipt->as_object(), "state"));
                if ((state && *state == "delivered") || truthy(find(receipt->as_object(), "delta"))) {
                    try {
                        part_count = validate_delivered_part(
                            receipt->as_object(), request_id, view_id, snapshot_id,
                            delivered_parts, part_count);
                    } catch (const std::exception& failure) {
                        throw InterfaceError(failure.what());
                    }
                    ++delivered_parts;
                    on_part(*result);
                }
            }
            continue;
        }
        if (!status || *status != "pending") {
            if (const auto* count_value = integer(find(*result, "model_trace_count"))) {
                if (*count_value < 0) throw InterfaceError("invalid_model_trace_count");
                auto collected = collect_model_trace(
                    request_id, required_string(*result, "view_id"),
                    static_cast<std::size_t>(*count_value), trace_sink);
                result->insert_or_assign("model_step_receipts", std::move(collected));
                result->insert_or_assign("model_trace_delivered", true);
                result->insert_or_assign("model_trace_streamed", static_cast<bool>(trace_sink));
            }
            (void)request("cognitive_dialogue_release", binding);
            return *result;
        }
        if (std::chrono::steady_clock::now() - started >= std::chrono::seconds(120))
            throw InterfaceError("dialogue_pending_resume_or_release_same_request_id");
        const auto* continuation = boolean(find(admitted, "memory_continuation_available"));
        if (continuation && *continuation)
            (void)request("cognitive_dialogue_continue", binding);
        else (void)request("cognitive_dialogue_tick");
        result.reset();
    }
}

AgentFacade::AgentFacade(
    std::shared_ptr<ResidentClient> resident_value,
    std::shared_ptr<ProviderRegistry> providers_value)
    : resident(std::move(resident_value)), providers(std::move(providers_value)) {
    if (!resident || !providers) throw InterfaceError("invalid_interface_config");
}

JsonValue::Object AgentFacade::status() const {
    const auto reply = resident->request("status");
    const auto* pair = string(find(reply, "pair_snapshot_id"));
    if (!pair || pair->size() != 64 ||
        !std::ranges::all_of(*pair, [](const char byte) {
            return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f');
        })) throw InterfaceError("resident_snapshot_unavailable");
    JsonValue resident_status;
    if (const auto* value = find(reply, "status")) resident_status = *value;
    JsonValue hot_count;
    if (const auto* value = find(reply, "hot_episode_count")) hot_count = *value;
    JsonValue lookup_io;
    if (const auto* value = find(reply, "lookup_requires_io")) lookup_io = *value;
    const auto* scheduled = boolean(find(reply, "scheduled_dialogue_available"));
    return {{"pair_snapshot_id", *pair}, {"resident_status", resident_status},
            {"scheduled_dialogue_available", scheduled && *scheduled},
            {"hot_episode_count", hot_count}, {"lookup_requires_io", lookup_io},
            {"authority", proposal_authority_false()}, {"observation_only", true}};
}

JsonValue::Object AgentFacade::respond(
    std::string query, std::string profile, std::string request_id,
    std::string expected_pair_snapshot_id, std::string dialogue_mode,
    Observer on_part) const {
    query = checked_text(std::move(query), 4096);
    profile = checked_text(std::move(profile), 128);
    request_id = checked_text(std::move(request_id), 128);
    if (dialogue_mode != "memory_report" && dialogue_mode != "conversation")
        throw InterfaceError("invalid_dialogue_mode");
    const auto before = status();
    if (required_string(before, "pair_snapshot_id") != expected_pair_snapshot_id)
        throw InterfaceError("snapshot_mismatch");
    JsonValue::Object arguments{{"query", query}, {"profile", profile},
        {"request_id", request_id},
        {"expected_pair_snapshot_id", expected_pair_snapshot_id}};
    if (dialogue_mode != "memory_report") arguments.emplace("dialogue_mode", dialogue_mode);
    const auto* scheduled = boolean(find(before, "scheduled_dialogue_available"));
    if (on_part && (!scheduled || !*scheduled))
        throw InterfaceError("incremental_dialogue_unavailable");
    auto result = scheduled && *scheduled
        ? resident->scheduled_dialogue(arguments, {}, std::move(on_part))
        : resident->request("cognitive_dialogue", arguments);
    if (required_string(status(), "pair_snapshot_id") != expected_pair_snapshot_id)
        throw InterfaceError("snapshot_changed_retry_explicitly");
    const auto* result_status = string(find(result, "status"));
    static const std::set<std::string, std::less<>> allowed{
        "main_vrs_dialogue_complete", "main_vrs_dialogue_incomplete",
        "main_vrs_dialogue_failed", "stale_proposal"};
    if (!result_status || !allowed.contains(*result_status))
        throw InterfaceError("main_dialogue_unavailable");
    return result;
}

JsonValue::Object AgentFacade::lookup(std::string cue) const {
    cue = checked_text(std::move(cue), 4096);
    const auto before = required_string(status(), "pair_snapshot_id");
    const auto reply = resident->request("cue", {{"cue", cue}});
    if (required_string(status(), "pair_snapshot_id") != before)
        throw InterfaceError("snapshot_changed_retry_explicitly");
    JsonValue candidate_count;
    if (const auto* value = find(reply, "candidate_count")) candidate_count = *value;
    JsonValue episode_ids(JsonValue::Array{});
    if (const auto* value = find(reply, "episode_ids")) episode_ids = *value;
    JsonValue lookup_io;
    if (const auto* value = find(reply, "lookup_requires_io")) lookup_io = *value;
    return {{"pair_snapshot_id", before}, {"cue", cue},
            {"candidate_count", candidate_count}, {"episode_ids", episode_ids},
            {"lookup_requires_io", lookup_io}, {"observation_only", true},
            {"four_stage_judgment_performed", false},
            {"authority", proposal_authority_false()}};
}

JsonValue::Object AgentFacade::propose(
    std::string profile, std::string request_id, std::string source_episode_id,
    std::string source_revision, std::string expected_pair_snapshot_id,
    std::string prompt, JsonValue::Object current_evidence) const {
    if (required_string(status(), "pair_snapshot_id") != expected_pair_snapshot_id)
        throw InterfaceError("snapshot_mismatch");
    auto request_value = DetachedProposalRequest::detach(
        checked_text(std::move(request_id), 128),
        checked_text(std::move(source_episode_id), 256),
        expected_pair_snapshot_id, checked_text(std::move(source_revision), 256),
        checked_text(std::move(prompt)), std::move(current_evidence));
    auto result = providers->propose(profile, request_value);
    if (required_string(status(), "pair_snapshot_id") != expected_pair_snapshot_id) {
        result.insert_or_assign("status", "stale_proposal");
        result.insert_or_assign("proposal", JsonValue(nullptr));
        result.insert_or_assign("error", "snapshot_changed");
    }
    return result;
}

AgentFacade load_facade(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw InterfaceError("invalid_interface_config");
    std::string bytes((std::istreambuf_iterator<char>(stream)),
                      std::istreambuf_iterator<char>());
    if (bytes.size() > provider_max_bytes) throw InterfaceError("payload_too_large");
    const auto config = provider_decode_object(bytes);
    static const std::set<std::string, std::less<>> expected{
        "schema_version", "resident_socket", "profiles"};
    if (config.size() != expected.size() ||
        std::ranges::any_of(expected, [&](const auto& key) { return !config.contains(key); }) ||
        !integer(find(config, "schema_version")) ||
        *integer(find(config, "schema_version")) != 1)
        throw InterfaceError("invalid_interface_config");
    const auto* profiles_value = find(config, "profiles");
    if (!profiles_value || !profiles_value->is_object() ||
        profiles_value->as_object().size() > 32)
        throw InterfaceError("invalid_profiles");
    std::vector<ProviderProfile> profiles;
    for (const auto& [name, specification] : profiles_value->as_object()) {
        if (!specification.is_object()) throw InterfaceError("invalid_profile");
        profiles.push_back(ProviderProfile::from_object(name, specification.as_object()));
    }
    return AgentFacade(
        std::make_shared<ResidentClient>(profile_text(config, "resident_socket")),
        std::make_shared<ProviderRegistry>(std::move(profiles)));
}

}  // namespace swegca::world
