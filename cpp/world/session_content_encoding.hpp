#pragma once

#include "world/session_semantic_binding.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view session_content_encoding_source_sha256 =
    "e62419a5fec3ea7b5356e26949501d08459443ba3932183212da389e23b95d2b";

class SessionContentEncoding final {
public:
    explicit SessionContentEncoding(std::shared_ptr<const BoundSessionSemantics> binding);

    const std::shared_ptr<const BoundSessionSemantics> binding;

    [[nodiscard]] constexpr std::string_view source_kind() const noexcept {
        return "session_document";
    }
    [[nodiscard]] std::string_view source_id() const;
    [[nodiscard]] std::string_view source_revision() const;
    [[nodiscard]] std::string_view source_digest() const;
    [[nodiscard]] std::vector<std::string> source_episode_ids() const;
    [[nodiscard]] const std::vector<std::string>& source_addresses() const;
    [[nodiscard]] const std::vector<SemanticAnchor>& anchors() const;
    [[nodiscard]] std::vector<SemanticMeaningUnit> units() const;
    [[nodiscard]] const std::vector<std::string>& unresolved() const;
    [[nodiscard]] const std::vector<std::string>& document_key() const;
    [[nodiscard]] constexpr std::vector<std::size_t> partial_response_units() const {
        return {};
    }
};

[[nodiscard]] std::vector<std::vector<SessionSemanticEdgeRole>>
claim_graph_addresses(const BoundSessionSemantics& bound);

}  // namespace swegca::world
