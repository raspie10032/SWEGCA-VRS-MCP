#pragma once

#include "world/cognitive_state.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view vrs_dialogue_source_sha256 =
    "1ffd50d21b8860eed11532ad8c670633d70c40fd2b9c3ee309f3a5b79fa59759";

[[nodiscard]] std::vector<std::string> vrs_dialogue_query_keys(std::string_view query);

struct VrsDialogueTurn final {
    std::string query;
    std::string utterance;
    std::string status;
    std::optional<std::string> selected_term;
    std::vector<std::string> candidate_terms;
    std::vector<std::string> rejected_terms;
    std::vector<JsonValue::Object> evidence;
    std::string selection_method;
    bool semantic_authority{};
    bool world_authority{};
    bool training_authority{};
    bool external_action_authority{};
    bool p3_authority{};
};

class VrsDialogueSession final {
public:
    VrsDialogueSession(
        std::vector<std::string> terms,
        std::vector<float> score,
        std::vector<std::uint64_t> support,
        std::vector<std::uint64_t> refute,
        std::vector<std::uint8_t> source_bits,
        std::vector<std::uint32_t> edge_source,
        std::vector<std::uint32_t> edge_target,
        std::vector<std::int8_t> edge_sign,
        std::vector<float> vrs_strength,
        std::map<std::string, JsonValue::Object, std::less<>> definitions,
        std::map<std::string, JsonValue::Object, std::less<>> trilingual,
        std::map<std::string, std::uint64_t, std::less<>> experience_stats);

    [[nodiscard]] VrsDialogueTurn respond(std::string query);
    [[nodiscard]] const std::vector<VrsDialogueTurn>& history() const noexcept;

private:
    [[nodiscard]] std::pair<std::vector<std::string>, std::vector<std::string>>
        associations(std::size_t term_id, std::size_t limit = 5) const;
    [[nodiscard]] std::vector<std::string> matches(std::string_view query) const;
    [[nodiscard]] std::vector<JsonValue::Object> term_evidence(std::string_view term) const;
    [[nodiscard]] std::string render_term(
        std::string_view term, const std::vector<JsonValue::Object>& evidence) const;

    std::vector<std::string> terms_;
    std::vector<float> score_;
    std::vector<std::uint64_t> support_;
    std::vector<std::uint64_t> refute_;
    std::vector<std::uint8_t> source_bits_;
    std::map<std::string, std::size_t, std::less<>> term_ids_;
    std::map<std::string, JsonValue::Object, std::less<>> definitions_;
    std::map<std::string, JsonValue::Object, std::less<>> trilingual_;
    std::map<std::string, std::uint64_t, std::less<>> experience_stats_;
    std::vector<std::size_t> offsets_;
    std::vector<std::uint32_t> neighbors_;
    std::vector<std::int8_t> signs_;
    std::vector<float> strengths_;
    std::vector<VrsDialogueTurn> history_;
};

}  // namespace swegca::world
