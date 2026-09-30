#pragma once

#include <optional>
#include "world/provider_transport.hpp"

#include <string>

namespace swegca::world {

// Safe typed local-provider failure. Only retained model text may be carried;
// transport envelopes, URLs and credentials never cross this boundary.
class SemanticResponseError final : public InterfaceError {
public:
    explicit SemanticResponseError(
        std::string code,
        std::optional<std::string> content = std::nullopt,
        std::optional<std::string> finish_reason = std::nullopt);

    const std::string failure_code;
    const std::string finish_reason;
    const std::optional<std::string> response_utf8;
};

}  // namespace swegca::world
