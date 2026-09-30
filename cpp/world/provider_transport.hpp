#pragma once

#include "world/cognitive_state.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>

namespace swegca::world {

inline constexpr std::size_t provider_max_bytes = 1'048'576;

class InterfaceError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct ProviderProfile final {
    std::string name;
    std::string protocol;
    std::string model;
    std::string endpoint;
    std::optional<std::string> api_key_env;
    bool allow_external{};
    bool enabled{};
    double timeout_seconds{30.0};
    std::string parameters_utf8{"{}"};

    ProviderProfile(
        std::string name, std::string protocol, std::string model,
        std::string endpoint, std::optional<std::string> api_key_env = std::nullopt,
        bool allow_external = false, bool enabled = false,
        double timeout_seconds = 30.0, std::string parameters_utf8 = "{}");

    [[nodiscard]] static ProviderProfile from_object(
        std::string name, const JsonValue::Object& specification);
};

[[nodiscard]] std::string provider_encode(const JsonValue& value);
[[nodiscard]] JsonValue provider_decode(std::string_view bytes);
[[nodiscard]] JsonValue::Object provider_decode_object(std::string_view bytes);
[[nodiscard]] bool provider_endpoint_is_loopback(std::string_view endpoint);

using ProviderPost = std::function<JsonValue::Object(
    const ProviderProfile&, const JsonValue::Object&)>;

// Transport only: no redirects, environment proxies, streaming, retries or
// remote tools. The returned JSON object carries no cognitive authority.
[[nodiscard]] JsonValue::Object post_provider_json(
    const ProviderProfile& profile, const JsonValue::Object& body);

}  // namespace swegca::world
