#pragma once

#include "world/parallel_experience_transport.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view language_generation_source_sha256 =
    "be0f52dec13d456b7f06cb5769027f4e7aa82c34c7ff4787cb53e71a3f130e9e";
inline constexpr std::int64_t text_bos_id = 257;
inline constexpr std::int64_t text_eos_id = 258;

class LanguageGenerationBackend {
public:
    virtual ~LanguageGenerationBackend() = default;
    [[nodiscard]] virtual std::vector<std::string> generate_batch(
        const std::vector<DetachedProposalRequest>& requests) = 0;
};

using LanguageTokenBatch = std::vector<std::vector<std::int64_t>>;

[[nodiscard]] LanguageTokenBatch generate_language_proposal(
    LanguageGenerationBackend& backend, const LanguageTokenBatch& input_ids,
    std::size_t maximum_new_bytes,
    const std::vector<DetachedProposalRequest>& requests,
    const std::function<std::string()>& current_snapshot_id);

}  // namespace swegca::world
