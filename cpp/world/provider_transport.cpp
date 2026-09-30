#include "world/provider_transport.hpp"

#include "transport/json.hpp"
#include "world/provider_cancellation.hpp"
#include "world/semantic_vrs_ingress.hpp"

#include <algorithm>
#include <array>
#include <arpa/inet.h>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <memory_resource>
#include <memory>
#include <netdb.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <poll.h>
#include <regex>
#include <set>
#include <span>
#include <stdexcept>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace swegca::world {
namespace {

struct Endpoint final {
    std::string scheme;
    std::string host;
    std::string port;
    std::string path;
    bool loopback{};
};

bool has_text(const std::string_view value) {
    return std::ranges::any_of(value, [](const unsigned char byte) {
        return byte != ' ' && byte != '\t' && byte != '\n' && byte != '\r' &&
               byte != '\f' && byte != '\v';
    });
}

bool valid_name(const std::string_view value) {
    if (value.empty() || value.size() > 128 ||
        !((value.front() >= 'A' && value.front() <= 'Z') ||
          (value.front() >= 'a' && value.front() <= 'z') ||
          (value.front() >= '0' && value.front() <= '9'))) return false;
    return std::ranges::all_of(value, [](const unsigned char byte) {
        return (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
               (byte >= '0' && byte <= '9') || byte == '_' || byte == '.' || byte == '-';
    });
}

bool valid_env_name(const std::string_view value) {
    if (value.empty() || !((value.front() >= 'A' && value.front() <= 'Z') ||
                           (value.front() >= 'a' && value.front() <= 'z') ||
                           value.front() == '_')) return false;
    return std::ranges::all_of(value.substr(1), [](const unsigned char byte) {
        return (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
               (byte >= '0' && byte <= '9') || byte == '_';
    });
}

bool literal_loopback(const std::string_view host) {
    in_addr ipv4{};
    if (::inet_pton(AF_INET, std::string(host).c_str(), &ipv4) == 1)
        return (ntohl(ipv4.s_addr) >> 24U) == 127U;
    in6_addr ipv6{};
    if (::inet_pton(AF_INET6, std::string(host).c_str(), &ipv6) == 1)
        return IN6_IS_ADDR_LOOPBACK(&ipv6);
    return false;
}

Endpoint parse_endpoint(const std::string_view endpoint) {
    if (endpoint.empty() || endpoint.size() > 4096 ||
        std::ranges::any_of(endpoint, [](const unsigned char byte) {
            return byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r';
        })) throw InterfaceError("invalid_endpoint");
    static const std::regex pattern(
        R"(^((?:http)|(?:https))://(\[[0-9A-Fa-f:.]+\]|[^/:?#]+)(?::([0-9]+))?(/[^?#]*)?$)",
        std::regex::ECMAScript);
    std::match_results<std::string_view::const_iterator> match;
    if (!std::regex_match(endpoint.begin(), endpoint.end(), match, pattern))
        throw InterfaceError("invalid_endpoint");
    Endpoint result;
    result.scheme = match[1].str();
    result.host = match[2].str();
    if (result.host.size() >= 2 && result.host.front() == '[' && result.host.back() == ']')
        result.host = result.host.substr(1, result.host.size() - 2);
    result.port = match[3].matched ? match[3].str() :
        result.scheme == "https" ? "443" : "80";
    unsigned port{};
    const auto parsed = std::from_chars(
        result.port.data(), result.port.data() + result.port.size(), port);
    if (parsed.ec != std::errc{} || parsed.ptr != result.port.data() + result.port.size() ||
        port == 0 || port > 65535) throw InterfaceError("invalid_endpoint");
    result.path = match[4].matched ? match[4].str() : "/";
    result.loopback = literal_loopback(result.host);
    return result;
}

JsonValue detached_json(const transport::Json& source) {
    switch (source.kind) {
    case transport::Json::Kind::null: return JsonValue(nullptr);
    case transport::Json::Kind::boolean: return JsonValue(source.scalar == "true");
    case transport::Json::Kind::string: return JsonValue(source.scalar);
    case transport::Json::Kind::number: {
        std::int64_t integer{};
        const auto parsed = std::from_chars(
            source.scalar.data(), source.scalar.data() + source.scalar.size(), integer);
        if (parsed.ec == std::errc{} &&
            parsed.ptr == source.scalar.data() + source.scalar.size()) return JsonValue(integer);
        if (source.scalar.find_first_of(".eE") == std::string_view::npos)
            return JsonValue(JsonInteger{std::string(source.scalar)});
        double number{};
        const auto real = std::from_chars(
            source.scalar.data(), source.scalar.data() + source.scalar.size(), number,
            std::chars_format::general);
        if (real.ec != std::errc{} ||
            real.ptr != source.scalar.data() + source.scalar.size() || !std::isfinite(number))
            throw InterfaceError("invalid_json_object");
        return JsonValue(number);
    }
    case transport::Json::Kind::array: {
        JsonValue::Array output;
        output.reserve(source.values.size());
        for (const auto& value : source.values) output.push_back(detached_json(value));
        return JsonValue(std::move(output));
    }
    case transport::Json::Kind::object: {
        JsonValue::Object output;
        for (std::size_t index = 0; index < source.values.size(); ++index)
            output.emplace(source.keys[index], detached_json(source.values[index]));
        return JsonValue(std::move(output));
    }
    }
    throw InterfaceError("invalid_json_object");
}

const JsonValue* find(const JsonValue::Object& object, const std::string_view key) {
    const auto item = object.find(key);
    return item == object.end() ? nullptr : &item->second;
}

std::string string_field(const JsonValue::Object& object, const std::string_view key) {
    const auto* value = find(object, key);
    const auto* text = value ? std::get_if<std::string>(&value->storage()) : nullptr;
    if (!text) throw InterfaceError("invalid_profile");
    return *text;
}

bool bool_field(const JsonValue::Object& object, const std::string_view key,
                const bool fallback) {
    const auto* value = find(object, key);
    if (!value) return fallback;
    const auto* flag = std::get_if<bool>(&value->storage());
    if (!flag) throw InterfaceError("invalid_profile");
    return *flag;
}

double number_field(const JsonValue::Object& object, const std::string_view key,
                    const double fallback) {
    const auto* value = find(object, key);
    if (!value) return fallback;
    if (const auto* integer = std::get_if<std::int64_t>(&value->storage()))
        return static_cast<double>(*integer);
    if (const auto* number = std::get_if<double>(&value->storage())) return *number;
    throw InterfaceError("invalid_profile");
}

class Socket final {
public:
    explicit Socket(const int value = -1) noexcept : value_(value) {}
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& other) noexcept : value_(std::exchange(other.value_, -1)) {}
    Socket& operator=(Socket&& other) noexcept {
        if (this != &other) { reset(); value_ = std::exchange(other.value_, -1); }
        return *this;
    }
    ~Socket() { reset(); }
    [[nodiscard]] int get() const noexcept { return value_; }
    void reset() noexcept { if (value_ >= 0) ::close(value_); value_ = -1; }
private:
    int value_;
};

Socket connect_socket(const Endpoint& endpoint, const double timeout) {
    addrinfo hints{};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    addrinfo* raw{};
    if (::getaddrinfo(endpoint.host.c_str(), endpoint.port.c_str(), &hints, &raw) != 0)
        throw InterfaceError("provider_request_failed");
    std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> addresses(raw, ::freeaddrinfo);
    const auto milliseconds = static_cast<int>(std::ceil(timeout * 1000.0));
    for (auto* address = addresses.get(); address; address = address->ai_next) {
        Socket socket(::socket(address->ai_family, address->ai_socktype, address->ai_protocol));
        if (socket.get() < 0) continue;
        const int flags = ::fcntl(socket.get(), F_GETFL, 0);
        if (flags < 0 || ::fcntl(socket.get(), F_SETFL, flags | O_NONBLOCK) < 0) continue;
        const int status = ::connect(socket.get(), address->ai_addr, address->ai_addrlen);
        if (status < 0 && errno != EINPROGRESS) continue;
        if (status < 0) {
            pollfd wait{socket.get(), POLLOUT, 0};
            if (::poll(&wait, 1, milliseconds) <= 0) continue;
            int error{}; socklen_t size = sizeof(error);
            if (::getsockopt(socket.get(), SOL_SOCKET, SO_ERROR, &error, &size) < 0 || error)
                continue;
        }
        if (::fcntl(socket.get(), F_SETFL, flags) < 0) continue;
        const timeval duration{static_cast<time_t>(timeout),
            static_cast<suseconds_t>((timeout - std::floor(timeout)) * 1'000'000.0)};
        (void)::setsockopt(socket.get(), SOL_SOCKET, SO_RCVTIMEO, &duration, sizeof(duration));
        (void)::setsockopt(socket.get(), SOL_SOCKET, SO_SNDTIMEO, &duration, sizeof(duration));
        return socket;
    }
    throw InterfaceError("provider_request_failed");
}

class Stream final {
public:
    Stream(Socket socket, const Endpoint& endpoint)
        : socket_(std::move(socket)), secure_(endpoint.scheme == "https") {
        if (!secure_) return;
        context_.reset(::SSL_CTX_new(::TLS_client_method()));
        if (!context_ || ::SSL_CTX_set_default_verify_paths(context_.get()) != 1)
            throw InterfaceError("provider_request_failed");
        ::SSL_CTX_set_verify(context_.get(), SSL_VERIFY_PEER, nullptr);
        ssl_.reset(::SSL_new(context_.get()));
        if (!ssl_ || ::SSL_set_fd(ssl_.get(), socket_.get()) != 1 ||
            ::SSL_set_tlsext_host_name(ssl_.get(), endpoint.host.c_str()) != 1 ||
            ::SSL_set1_host(ssl_.get(), endpoint.host.c_str()) != 1 ||
            ::SSL_connect(ssl_.get()) != 1)
            throw InterfaceError("provider_request_failed");
    }

    [[nodiscard]] int socket() const noexcept { return socket_.get(); }
    void write_all(const std::string_view bytes) {
        std::size_t offset{};
        while (offset < bytes.size()) {
            const int count = secure_
                ? ::SSL_write(ssl_.get(), bytes.data() + offset,
                              static_cast<int>(std::min<std::size_t>(bytes.size() - offset, 1U << 20U)))
                : static_cast<int>(::send(socket_.get(), bytes.data() + offset,
                                         bytes.size() - offset, MSG_NOSIGNAL));
            if (count <= 0) throw InterfaceError("provider_request_failed");
            offset += static_cast<std::size_t>(count);
        }
    }
    std::size_t read(std::span<char> output) {
        const int count = secure_
            ? ::SSL_read(ssl_.get(), output.data(), static_cast<int>(output.size()))
            : static_cast<int>(::recv(socket_.get(), output.data(), output.size(), 0));
        if (count > 0) return static_cast<std::size_t>(count);
        if (count == 0) return 0;
        throw InterfaceError("provider_request_failed");
    }
private:
    struct CtxDelete { void operator()(SSL_CTX* value) const noexcept { ::SSL_CTX_free(value); } };
    struct SslDelete { void operator()(SSL* value) const noexcept { ::SSL_free(value); } };
    Socket socket_;
    bool secure_{};
    std::unique_ptr<SSL_CTX, CtxDelete> context_;
    std::unique_ptr<SSL, SslDelete> ssl_;
};

std::string lower(std::string value) {
    std::ranges::transform(value, value.begin(), [](const unsigned char byte) {
        return static_cast<char>(byte >= 'A' && byte <= 'Z' ? byte + ('a' - 'A') : byte);
    });
    return value;
}

std::string decode_chunked(const std::string_view body) {
    std::string output;
    std::size_t cursor{};
    while (true) {
        const auto line_end = body.find("\r\n", cursor);
        if (line_end == std::string_view::npos) throw InterfaceError("provider_request_failed");
        const auto token = body.substr(cursor, line_end - cursor);
        const auto extension = token.find(';');
        const auto digits = token.substr(0, extension);
        std::size_t size{};
        const auto parsed = std::from_chars(
            digits.data(), digits.data() + digits.size(), size, 16);
        if (digits.empty() || parsed.ec != std::errc{} ||
            parsed.ptr != digits.data() + digits.size())
            throw InterfaceError("provider_request_failed");
        cursor = line_end + 2;
        if (!size) return output;
        if (size > provider_max_bytes - std::min(output.size(), provider_max_bytes) ||
            cursor + size + 2 > body.size() || body.substr(cursor + size, 2) != "\r\n")
            throw InterfaceError("payload_too_large");
        output.append(body.substr(cursor, size));
        cursor += size + 2;
    }
}

std::pair<unsigned, std::string> parse_http_response(const std::string& response) {
    const auto header_end = response.find("\r\n\r\n");
    if (header_end == std::string::npos || header_end > 65536)
        throw InterfaceError("provider_request_failed");
    const auto status_end = response.find("\r\n");
    if (status_end == std::string::npos) throw InterfaceError("provider_request_failed");
    const auto first_space = response.find(' ');
    if (first_space == std::string::npos || first_space > status_end)
        throw InterfaceError("provider_request_failed");
    const auto second_space = response.find(' ', first_space + 1);
    const auto code_text = std::string_view(response).substr(
        first_space + 1,
        (second_space == std::string::npos || second_space > status_end ? status_end : second_space) -
            first_space - 1);
    unsigned code{};
    const auto parsed = std::from_chars(code_text.data(), code_text.data() + code_text.size(), code);
    if (parsed.ec != std::errc{} || parsed.ptr != code_text.data() + code_text.size())
        throw InterfaceError("provider_request_failed");
    bool chunked{};
    std::optional<std::size_t> content_length;
    std::size_t cursor = status_end + 2;
    while (cursor < header_end) {
        const auto end = response.find("\r\n", cursor);
        if (end == std::string::npos || end > header_end)
            throw InterfaceError("provider_request_failed");
        const auto colon = response.find(':', cursor);
        if (colon == std::string::npos || colon > end)
            throw InterfaceError("provider_request_failed");
        auto name = lower(response.substr(cursor, colon - cursor));
        auto value = response.substr(colon + 1, end - colon - 1);
        const auto begin = value.find_first_not_of(" \t");
        const auto finish = value.find_last_not_of(" \t");
        value = begin == std::string::npos ? "" : value.substr(begin, finish - begin + 1);
        if (name == "transfer-encoding") chunked = lower(value) == "chunked";
        if (name == "content-length") {
            std::size_t length{};
            const auto length_parsed = std::from_chars(
                value.data(), value.data() + value.size(), length);
            if (length_parsed.ec != std::errc{} ||
                length_parsed.ptr != value.data() + value.size() || content_length)
                throw InterfaceError("provider_request_failed");
            content_length = length;
        }
        cursor = end + 2;
    }
    const std::string_view raw_body(response.data() + header_end + 4,
                                    response.size() - header_end - 4);
    std::string body;
    if (chunked) body = decode_chunked(raw_body);
    else if (content_length) {
        if (*content_length > provider_max_bytes || raw_body.size() != *content_length)
            throw InterfaceError(*content_length > provider_max_bytes
                ? "payload_too_large" : "provider_request_failed");
        body.assign(raw_body);
    } else {
        if (raw_body.size() > provider_max_bytes) throw InterfaceError("payload_too_large");
        body.assign(raw_body);
    }
    return {code, std::move(body)};
}

}  // namespace

ProviderProfile::ProviderProfile(
    std::string name_value, std::string protocol_value, std::string model_value,
    std::string endpoint_value, std::optional<std::string> api_key_env_value,
    const bool allow_external_value, const bool enabled_value,
    const double timeout_seconds_value, std::string parameters_utf8_value)
    : name(std::move(name_value)), protocol(std::move(protocol_value)),
      model(std::move(model_value)), endpoint(std::move(endpoint_value)),
      api_key_env(std::move(api_key_env_value)), allow_external(allow_external_value),
      enabled(enabled_value), timeout_seconds(timeout_seconds_value),
      parameters_utf8(std::move(parameters_utf8_value)) {
    if (!valid_name(name) || !valid_name(protocol))
        throw InterfaceError("invalid_profile_name");
    if (!has_text(model) || model.size() > 256) throw InterfaceError("invalid_text");
    if (!std::isfinite(timeout_seconds) || timeout_seconds <= 0 || timeout_seconds > 300)
        throw InterfaceError("invalid_timeout");
    if (api_key_env && !valid_env_name(*api_key_env))
        throw InterfaceError("invalid_key_environment_name");
    (void)provider_decode_object(parameters_utf8);
    if (endpoint.empty() || endpoint.size() > 4096 || !has_text(endpoint))
        throw InterfaceError("invalid_text");
    if (protocol == "codex_exec") {
        if (endpoint.front() != '/' || api_key_env)
            throw InterfaceError("codex_absolute_executable_and_existing_login_required");
        if (!allow_external) throw InterfaceError("codex_requires_explicit_external_opt_in");
        if (!valid_name(model)) throw InterfaceError("invalid_codex_model");
        const auto parameters = provider_decode_object(parameters_utf8);
        if (parameters.size() > 1 ||
            (parameters.size() == 1 && !parameters.contains("reasoning_effort")))
            throw InterfaceError("invalid_codex_parameters");
        if (const auto* value = find(parameters, "reasoning_effort")) {
            const auto* effort = std::get_if<std::string>(&value->storage());
            static const std::set<std::string, std::less<>> allowed{
                "none", "low", "medium", "high", "xhigh", "max"};
            if (!effort || !allowed.contains(*effort))
                throw InterfaceError("invalid_codex_parameters");
        }
        return;
    }
    if (protocol != "chat_completions" && protocol != "ollama_chat") return;
    const auto parsed = parse_endpoint(endpoint);
    if (!parsed.loopback && (!allow_external || parsed.scheme != "https"))
        throw InterfaceError("external_endpoint_requires_explicit_https_opt_in");
}

ProviderProfile ProviderProfile::from_object(
    std::string name, const JsonValue::Object& specification) {
    static const std::set<std::string, std::less<>> allowed{
        "protocol", "model", "endpoint", "api_key_env", "allow_external",
        "enabled", "timeout_seconds", "parameters"};
    if (std::ranges::any_of(specification, [&](const auto& row) {
            return !allowed.contains(row.first);
        })) throw InterfaceError("unknown_profile_fields");
    try {
        std::optional<std::string> environment;
        if (const auto* value = find(specification, "api_key_env")) {
            if (std::holds_alternative<std::nullptr_t>(value->storage())) environment.reset();
            else if (const auto* text = std::get_if<std::string>(&value->storage())) environment = *text;
            else throw InterfaceError("invalid_profile");
        }
        JsonValue parameters(JsonValue::Object{});
        if (const auto* value = find(specification, "parameters")) parameters = *value;
        return ProviderProfile(
            std::move(name), string_field(specification, "protocol"),
            string_field(specification, "model"), string_field(specification, "endpoint"),
            std::move(environment), bool_field(specification, "allow_external", false),
            bool_field(specification, "enabled", false),
            number_field(specification, "timeout_seconds", 30.0),
            provider_encode(parameters));
    } catch (const InterfaceError&) { throw; }
    catch (const std::exception&) { throw InterfaceError("invalid_profile"); }
}

std::string provider_encode(const JsonValue& value) {
    auto output = semantic_canonical_json(value);
    if (output.size() > provider_max_bytes) throw InterfaceError("payload_too_large");
    return output;
}

JsonValue::Object provider_decode_object(const std::string_view bytes) {
    auto value = provider_decode(bytes);
    if (!value.is_object()) throw InterfaceError("invalid_json_object");
    return value.as_object();
}

JsonValue provider_decode(const std::string_view bytes) {
    if (bytes.size() > provider_max_bytes) throw InterfaceError("payload_too_large");
    try {
        std::pmr::monotonic_buffer_resource memory;
        auto value = detached_json(transport::parse_json(bytes, memory));
        return value;
    } catch (const std::bad_alloc&) { throw; }
    catch (const InterfaceError&) { throw; }
    catch (const std::exception&) { throw InterfaceError("invalid_json_object"); }
}

bool provider_endpoint_is_loopback(const std::string_view endpoint) {
    return parse_endpoint(endpoint).loopback;
}

JsonValue::Object post_provider_json(
    const ProviderProfile& profile, const JsonValue::Object& body) {
    check_cancelled();
    const auto endpoint = parse_endpoint(profile.endpoint);
    const auto payload = provider_encode(JsonValue(body));
    std::optional<std::string> key;
    if (profile.api_key_env) {
        const char* value = std::getenv(profile.api_key_env->c_str());
        if (!value || !*value || std::strchr(value, '\r') || std::strchr(value, '\n'))
            throw InterfaceError("provider_key_unavailable");
        key = value;
    }
    try {
        Stream stream(connect_socket(endpoint, profile.timeout_seconds), endpoint);
        auto registration = current_cancellation()
            ? std::optional<ProviderCancellation::InterruptRegistration>(
                current_cancellation()->interruptible([socket = stream.socket()] {
                    (void)::shutdown(socket, SHUT_RDWR);
                }))
            : std::nullopt;
        std::string host = endpoint.host;
        if (host.find(':') != std::string::npos) host = "[" + host + "]";
        const bool default_port = (endpoint.scheme == "http" && endpoint.port == "80") ||
                                  (endpoint.scheme == "https" && endpoint.port == "443");
        std::string request = "POST " + endpoint.path + " HTTP/1.1\r\nHost: " + host +
            (default_port ? "" : ":" + endpoint.port) +
            "\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " +
            std::to_string(payload.size()) + "\r\n";
        if (key) request += "Authorization: Bearer " + *key + "\r\n";
        request += "\r\n";
        request += payload;
        stream.write_all(request);
        std::string response;
        std::array<char, 16'384> buffer{};
        while (true) {
            const auto count = stream.read(buffer);
            if (!count) break;
            if (response.size() + count > provider_max_bytes + 65'536)
                throw InterfaceError("payload_too_large");
            response.append(buffer.data(), count);
        }
        const auto [status, response_body] = parse_http_response(response);
        if (status >= 400 && status <= 599)
            throw InterfaceError("provider_http_" + std::to_string(status));
        if (status != 200) throw InterfaceError("provider_http_failure");
        auto result = provider_decode_object(response_body);
        check_cancelled();
        return result;
    } catch (const std::bad_alloc&) { throw; }
    catch (const InterfaceError& failure) {
        const std::string code = failure.what();
        if (code.starts_with("provider_http_") && code != "provider_http_failure") throw;
        if (code == "payload_too_large") throw;
        throw InterfaceError("provider_request_failed");
    } catch (const std::exception&) {
        throw InterfaceError("provider_request_failed");
    }
}

}  // namespace swegca::world
