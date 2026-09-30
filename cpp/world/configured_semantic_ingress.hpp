#pragma once

#include "world/offline_semantic_batch.hpp"
#include "world/semantic_local_provider.hpp"

#include <optional>
#include <string_view>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view configured_semantic_ingress_source_sha256 =
    "dab3b5f47db25d6c8218e645014da4c5cae6369bb2123b5e66651c551a46b041";

[[nodiscard]] std::vector<SemanticDeliveredPart> authored_text_parts(
    const SemanticSourceEpisode& source);
[[nodiscard]] std::vector<SemanticDeliveredPart> source_text_parts(
    const SemanticSourceEpisode& source);

class ConfiguredSemanticIngress final {
public:
    ConfiguredSemanticIngress(
        bool authored_only, ProviderProfile profile,
        ProviderPost post = post_provider_json);

    [[nodiscard]] PreparedSemanticBatch operator()(
        const std::vector<SemanticSourceEpisode>& episodes) const;

private:
    bool authored_only_{};
    LocalSemanticProducer producer_;
};

// A missing configuration and a valid disabled configuration both return no
// callable. A disabled configuration still validates its profile and local
// transport before returning.
[[nodiscard]] std::optional<ConfiguredSemanticIngress>
configure_semantic_ingress(
    const std::optional<JsonValue>& configuration,
    ProviderPost post = post_provider_json);

}  // namespace swegca::world
