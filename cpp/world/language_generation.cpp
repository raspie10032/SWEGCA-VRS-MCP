#include "world/language_generation.hpp"

#include <algorithm>
#include <stdexcept>

namespace swegca::world {

LanguageTokenBatch generate_language_proposal(
    LanguageGenerationBackend& backend, const LanguageTokenBatch& input_ids,
    const std::size_t maximum_new_bytes,
    const std::vector<DetachedProposalRequest>& requests,
    const std::function<std::string()>& current_snapshot_id) {
    if (input_ids.empty() || input_ids.front().size() < 2 || !current_snapshot_id)
        throw std::invalid_argument("language backend requires a nonempty int64 text batch");
    const auto input_width = input_ids.front().size();
    if (std::ranges::any_of(input_ids, [&](const auto& row) {
            return row.size() != input_width;
        }))
        throw std::invalid_argument("language backend requires a rectangular text batch");
    if (requests.size() != input_ids.size())
        throw std::invalid_argument("one detached request is required per input row");
    const auto snapshot_id = current_snapshot_id();
    for (std::size_t row = 0; row < input_ids.size(); ++row) {
        const auto& ids = input_ids[row];
        if (ids.front() != text_bos_id)
            throw std::invalid_argument("language request must match the exact BOS-prefixed input bytes");
        std::string bytes;
        bytes.reserve(ids.size() - 1);
        for (std::size_t column = 1; column < ids.size(); ++column) {
            if (ids[column] < 0 || ids[column] > 255)
                throw std::invalid_argument("language request must match the exact BOS-prefixed input bytes");
            bytes.push_back(static_cast<char>(static_cast<unsigned char>(ids[column])));
        }
        if (bytes != requests[row].prompt_utf8 ||
            requests[row].full_current_pair_snapshot_id != snapshot_id)
            throw std::invalid_argument("stale or byte-mismatched language request");
    }
    if (maximum_new_bytes == 0) return input_ids;
    auto outputs = backend.generate_batch(requests);
    if (current_snapshot_id() != snapshot_id)
        throw std::invalid_argument("main snapshot changed during language generation");
    if (outputs.size() != requests.size())
        throw std::invalid_argument("language backend output batch size changed");
    std::vector<std::vector<std::int64_t>> suffixes;
    std::size_t suffix_width = 0;
    for (const auto& output : outputs) {
        if (output.empty() || std::ranges::all_of(output, [](const unsigned char value) {
                return value == ' ' || value == '\t' || value == '\r' || value == '\n';
            }))
            throw std::invalid_argument("language backend returned an empty/non-text proposal");
        if (output.size() > maximum_new_bytes)
            throw std::invalid_argument("language proposal exceeds byte budget; not truncated");
        std::vector<std::int64_t> suffix;
        suffix.reserve(output.size() + 1);
        for (const unsigned char byte : output) suffix.push_back(byte);
        if (suffix.size() < maximum_new_bytes) suffix.push_back(text_eos_id);
        suffix_width = std::max(suffix_width, suffix.size());
        suffixes.push_back(std::move(suffix));
    }
    LanguageTokenBatch result = input_ids;
    for (std::size_t row = 0; row < result.size(); ++row) {
        suffixes[row].resize(suffix_width, text_eos_id);
        result[row].insert(result[row].end(), suffixes[row].begin(), suffixes[row].end());
    }
    return result;
}

}  // namespace swegca::world
