#pragma once

#include "world/provider_transport.hpp"
#include "world/semantic_encoding.hpp"
#include "world/semantic_response_error.hpp"

#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view semantic_local_provider_source_sha256 =
    "8471fcd06b999cffdfe9ee4ddfff7e53a73e7876aff4110b7db2dd765cab7893";

[[nodiscard]] JsonValue semantic_response_schema(
    const SemanticEncodingInput& request,
    bool allow_table_values = false,
    bool allow_speech_values = false,
    bool speech_events_only = false);

class LocalSemanticProducer final {
public:
    LocalSemanticProducer(
        ProviderProfile profile,
        std::vector<std::string> modalities = {"text", "image"},
        ProviderPost post = post_provider_json,
        bool allow_table_values = false,
        bool allow_speech_values = false,
        bool speech_events_only = false);

    [[nodiscard]] JsonValue::Object request_body(
        const SemanticEncodingInput& request) const;
    [[nodiscard]] SemanticProducerResult operator()(
        const SemanticEncodingInput& request) const;

    const ProviderProfile profile;
    const std::set<std::string, std::less<>> modalities;
    const ProviderPost post;
    const bool allow_table_values;
    const bool allow_speech_values;
    const bool speech_events_only;
};

}  // namespace swegca::world
