#include "vrs_state_update.hpp"

#include "unicode.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace swegca::vrs {
namespace {

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_state_update.py@7536139:25-27
bool nonblank(std::string_view value) {
    const auto points = decode_utf8(value);
    return std::any_of(points.begin(), points.end(),
                       [](auto point) { return !python_space(point); });
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_state_update.py@7536139:99-113
std::string trimmed_numeric(std::string_view value) {
    const auto points = decode_utf8(value);
    std::size_t first = 0, last = points.size();
    while (first < last && python_space(points[first])) ++first;
    while (last > first && python_space(points[last - 1])) --last;
    std::string result;
    for (std::size_t at = first; at < last; ++at) append_utf8(result, points[at]);
    return result;
}

// Python int() accepts underscore-separated decimal text. Keep the exact
// canonical decimal address without constraining it to a machine word.
// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_state_update.py@7536139:112-113
std::string decimal_address(std::string_view text) {
    auto value = trimmed_numeric(text);
    bool negative = false;
    std::size_t at = 0;
    if (at < value.size() && (value[at] == '+' || value[at] == '-')) {
        negative = value[at] == '-';
        ++at;
    }
    if (at == value.size()) throw std::runtime_error("invalid connection address");
    std::string digits;
    bool previous_digit = false;
    for (; at < value.size(); ++at) {
        const char c = value[at];
        if (c == '_') {
            if (!previous_digit || at + 1 == value.size() ||
                value[at + 1] < '0' || value[at + 1] > '9')
                throw std::runtime_error("invalid connection address");
            previous_digit = false;
        } else if (c >= '0' && c <= '9') {
            digits.push_back(c);
            previous_digit = true;
        } else throw std::runtime_error("invalid connection address");
    }
    const auto first_digit = digits.find_first_not_of('0');
    if (first_digit == std::string::npos) return "0";
    return std::string(negative ? "-" : "") + digits.substr(first_digit);
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_state_update.py@7536139:112-113
std::string integer_address(const Json& value) {
    if (const auto integer = std::get_if<std::int64_t>(&value.data))
        return std::to_string(*integer);
    if (const auto boolean = std::get_if<bool>(&value.data))
        return *boolean ? "1" : "0";
    if (const auto number = std::get_if<double>(&value.data)) {
        if (!std::isfinite(*number)) throw std::runtime_error("invalid connection address");
        const auto truncated = std::trunc(*number);
        if (truncated == 0) return "0";
        std::ostringstream decimal;
        decimal.imbue(std::locale::classic());
        decimal << std::fixed << std::setprecision(0) << truncated;
        return decimal.str();
    }
    if (const auto text = std::get_if<std::string>(&value.data))
        return decimal_address(*text);
    throw std::runtime_error("invalid connection address");
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_state_update.py@7536139:99-99
double observation_strength(const Json& value) {
    if (const auto number = std::get_if<double>(&value.data)) return *number;
    if (const auto integer = std::get_if<std::int64_t>(&value.data))
        return static_cast<double>(*integer);
    if (const auto boolean = std::get_if<bool>(&value.data)) return *boolean ? 1.0 : 0.0;
    if (const auto text = std::get_if<std::string>(&value.data)) {
        auto numeric = trimmed_numeric(*text);
        if (numeric.empty()) throw std::runtime_error("invalid connection strength");
        std::string compact;
        compact.reserve(numeric.size());
        for (std::size_t at = 0; at < numeric.size(); ++at) {
            if (numeric[at] == '_') {
                if (at == 0 || at + 1 == numeric.size() ||
                    numeric[at - 1] < '0' || numeric[at - 1] > '9' ||
                    numeric[at + 1] < '0' || numeric[at + 1] > '9')
                    throw std::runtime_error("invalid connection strength");
            } else compact.push_back(numeric[at]);
        }
        if (!compact.empty() && compact.front() == '+') {
            compact.erase(0, 1);
            if (compact.empty() || compact.front() == '-' || compact.front() == '+')
                throw std::runtime_error("invalid connection strength");
        }
        double parsed = 0;
        const auto result = std::from_chars(compact.data(), compact.data() + compact.size(), parsed);
        if (result.ec != std::errc{} || result.ptr != compact.data() + compact.size())
            throw std::runtime_error("invalid connection strength");
        return parsed;
    }
    throw std::runtime_error("invalid connection strength");
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_state_update.py@7536139:85-94
const MemoryStep* first_connection_step(const ReplayedEpisode& episode) {
    for (const auto& step : episode.steps) {
        const auto& observation = step.observation.object();
        if ((observation.contains("edge_id") && observation.contains("vrs_strength")) ||
            (observation.contains("canonical_group_id") &&
             observation.contains("deweighted_vrs_strength")))
            return &step;
    }
    return nullptr;
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_state_update.py@7536139:103-111
std::pair<double, std::string> proposed_strength(
    const CurrentEvidenceVerdict& judgment, double previous,
    const std::set<std::string>& conflicts) {
    if (conflicts.contains(judgment.proposition)) return {previous, "abstain_conflict"};
    if (judgment.verdict == "support") return {previous * 1.01, "reinforce"};
    if (judgment.verdict == "refute") return {previous * 0.995, "weaken"};
    return {previous, "preserve_unresolved"};
}

}  // namespace

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_state_update.py@7536139:11-34
VRSConnectionStateUpdate::VRSConnectionStateUpdate(
    std::string new_episode_id, std::string new_connection_id,
    std::string new_proposition, std::string new_verdict,
    double new_previous_strength, double new_current_strength,
    std::string new_update_action, VRSExperiencePromotionDecision new_promotion,
    bool new_underlying_experience_preserved,
    std::vector<std::tuple<std::string, std::string, std::string>> new_source_judgments)
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_state_update.py@7536139:11-34
    : episode_id(std::move(new_episode_id)), connection_id(std::move(new_connection_id)),
      proposition(std::move(new_proposition)), verdict(std::move(new_verdict)),
      previous_strength(new_previous_strength), current_strength(new_current_strength),
      update_action(std::move(new_update_action)), promotion(std::move(new_promotion)),
      underlying_experience_preserved(new_underlying_experience_preserved),
      source_judgments(std::move(new_source_judgments)) {
    if (!nonblank(episode_id) || !nonblank(connection_id) || !nonblank(proposition) ||
        !std::isfinite(previous_strength) || previous_strength < 0 ||
        !std::isfinite(current_strength) || current_strength < 0 ||
        !underlying_experience_preserved)
        throw std::runtime_error("VRS connection state update changed");
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_state_update.py@7536139:38-72
VRSStateUpdateReceipt::VRSStateUpdateReceipt(
    std::string new_snapshot_id, std::vector<VRSConnectionStateUpdate> new_updates,
    std::array<std::string, 5> new_stage_order, bool new_detached_state_only,
    bool new_persistent_state_mutated, bool new_action_authorized,
    bool new_persistent_write_authorized, bool new_semantic_promotion_authorized)
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_state_update.py@7536139:38-72
    : snapshot_id(std::move(new_snapshot_id)), updates(std::move(new_updates)),
      stage_order(std::move(new_stage_order)), detached_state_only(new_detached_state_only),
      persistent_state_mutated(new_persistent_state_mutated),
      action_authorized(new_action_authorized),
      persistent_write_authorized(new_persistent_write_authorized),
      semantic_promotion_authorized(new_semantic_promotion_authorized) {
    if (!nonblank(snapshot_id) ||
        stage_order != std::array<std::string, 5>{
            "deja_vu", "recall", "replay", "re_evidence", "vrs_state_update"} ||
        !detached_state_only || persistent_state_mutated || action_authorized ||
        persistent_write_authorized || semantic_promotion_authorized)
        throw std::runtime_error("VRS state update authority or identity changed");
    std::set<std::string> connections;
    for (const auto& row : updates)
        if (!connections.insert(row.connection_id).second)
            throw std::runtime_error("VRS state update authority or identity changed");
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_state_update.py@7536139:75-152
VRSStateUpdateReceipt plan_vrs_state_update(const MemoryActivationReceipt& receipt) {
    std::map<std::string, const ReplayedEpisode*> replayed;
    for (const auto& episode : receipt.replay.episodes) replayed[episode.episode_id] = &episode;
    const std::set<std::string> conflicts(
        receipt.re_evidence.conflicting_propositions.begin(),
        receipt.re_evidence.conflicting_propositions.end());
    std::vector<VRSConnectionStateUpdate> updates;
    for (const auto& judgment : receipt.re_evidence.judgments) {
        const auto found = replayed.find(judgment.episode_id);
        if (found == replayed.end()) throw std::runtime_error("replay judgment identity changed");
        const auto* step = first_connection_step(*found->second);
        if (!step) continue;
        const auto& observation = step->observation;
        const bool canonical = observation.contains("canonical_group_id");
        const auto previous = observation_strength(observation.at(
            canonical ? "deweighted_vrs_strength" : "vrs_strength"));
        const auto [current, action] = proposed_strength(judgment, previous, conflicts);
        const auto connection_id = std::string(canonical ? "vrs-edge-group:" : "vrs-edge:") +
            integer_address(observation.at(canonical ? "canonical_group_id" : "edge_id"));
        updates.emplace_back(
            found->second->episode_id, connection_id, judgment.proposition,
            judgment.verdict, previous, current, action,
            assess_vrs_experience_promotion(receipt.snapshot_id, connection_id,
                                            previous, current));
    }

    // Preserve first-seen connection order and all source judgments. Opposing
    // directions or an explicit conflict leave the original strength intact.
    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_state_update.py@7536139:131-151
    std::vector<std::string> order;
    std::map<std::string, std::vector<std::size_t>> grouped;
    for (std::size_t at = 0; at < updates.size(); ++at) {
        const auto& connection = updates[at].connection_id;
        if (!grouped.contains(connection)) order.push_back(connection);
        grouped[connection].push_back(at);
    }
    std::vector<VRSConnectionStateUpdate> unique;
    unique.reserve(order.size());
    for (const auto& connection : order) {
        const auto& rows = grouped.at(connection);
        const auto& first = updates[rows.front()];
        bool reinforce = false, weaken = false, explicit_conflict = false;
        const VRSConnectionStateUpdate* selected = &first;
        bool selected_direction = false;
        std::vector<std::tuple<std::string, std::string, std::string>> judgments;
        for (const auto at : rows) {
            const auto& row = updates[at];
            if (row.previous_strength != first.previous_strength)
                throw std::runtime_error("connection aliases disagree on current snapshot strength");
            reinforce |= row.update_action == "reinforce";
            weaken |= row.update_action == "weaken";
            explicit_conflict |= row.update_action == "abstain_conflict";
            if (!selected_direction && (row.update_action == "reinforce" ||
                                        row.update_action == "weaken")) {
                selected = &row;
                selected_direction = true;
            }
            judgments.emplace_back(row.episode_id, row.proposition, row.verdict);
        }
        const bool conflict = explicit_conflict || (reinforce && weaken);
        const double current = conflict ? first.previous_strength : selected->current_strength;
        unique.emplace_back(
            selected->episode_id, selected->connection_id, selected->proposition,
            selected->verdict, selected->previous_strength, current,
            conflict ? "abstain_conflict" : selected->update_action,
            assess_vrs_experience_promotion(receipt.snapshot_id, connection,
                                            first.previous_strength, current),
            selected->underlying_experience_preserved, std::move(judgments));
    }
    return VRSStateUpdateReceipt(receipt.snapshot_id, std::move(unique));
}

}  // namespace swegca::vrs
