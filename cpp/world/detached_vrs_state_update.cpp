#include "world/detached_vrs_state_update.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace swegca::world {
namespace {

[[nodiscard]] bool has_text(const std::string_view value) noexcept {
    return std::any_of(value.begin(), value.end(), [](const unsigned char byte) {
        return byte != ' ' && byte != '\t' && byte != '\n' && byte != '\r' && byte != '\f' && byte != '\v';
    });
}

[[nodiscard]] bool valid_strength(const double value) noexcept {
    return std::isfinite(value) && value >= 0.0;
}

[[nodiscard]] bool contains(
    const std::vector<std::string>& values, const std::string_view value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

struct Provisional final {
    VrsConnectionStateUpdate update;
};

}  // namespace

VrsExperiencePromotionDecision::VrsExperiencePromotionDecision(
    std::string snapshot_id_value, std::string connection_id_value,
    const double previous_strength_value, const double current_strength_value,
    std::string action_value, const bool promoted_value,
    const bool semantic_evidence_allowed_value,
    const bool underlying_experience_preserved_value,
    const bool action_authorized_value,
    const bool persistent_write_authorized_value)
    : snapshot_id(std::move(snapshot_id_value)), connection_id(std::move(connection_id_value)),
      previous_strength(previous_strength_value), current_strength(current_strength_value),
      action(std::move(action_value)), promoted(promoted_value),
      semantic_evidence_allowed(semantic_evidence_allowed_value),
      underlying_experience_preserved(underlying_experience_preserved_value),
      action_authorized(action_authorized_value),
      persistent_write_authorized(persistent_write_authorized_value) {
    if (!has_text(snapshot_id) || !has_text(connection_id) ||
        !valid_strength(previous_strength) || !valid_strength(current_strength)) {
        throw std::invalid_argument("VRS promotion requires valid identity and strengths");
    }
    const bool was_promoted = previous_strength >= 1.0;
    const bool is_promoted = current_strength >= 1.0;
    const std::string_view expected = was_promoted && is_promoted ? "retain" :
        was_promoted ? "revoke" : is_promoted ? "promote" : "remain_unpromoted";
    if (action != expected || promoted != is_promoted ||
        semantic_evidence_allowed != is_promoted) {
        throw std::invalid_argument("VRS promotion decision changed");
    }
    if (!underlying_experience_preserved || action_authorized || persistent_write_authorized) {
        throw std::invalid_argument("VRS promotion changed its authority boundary");
    }
}

VrsConnectionStateUpdate::VrsConnectionStateUpdate(
    std::string episode_id_value, std::string connection_id_value,
    std::string proposition_value, const CurrentEvidenceVerdict verdict_value,
    const double previous_strength_value, const double current_strength_value,
    std::string update_action_value, VrsExperiencePromotionDecision promotion_value,
    const bool underlying_experience_preserved_value,
    std::vector<VrsSourceJudgment> source_judgments_value)
    : episode_id(std::move(episode_id_value)), connection_id(std::move(connection_id_value)),
      proposition(std::move(proposition_value)), verdict(verdict_value),
      previous_strength(previous_strength_value), current_strength(current_strength_value),
      update_action(std::move(update_action_value)), promotion(std::move(promotion_value)),
      underlying_experience_preserved(underlying_experience_preserved_value),
      source_judgments(std::move(source_judgments_value)) {
    if (!has_text(episode_id) || !has_text(connection_id) || !has_text(proposition) ||
        !valid_strength(previous_strength) || !valid_strength(current_strength) ||
        !underlying_experience_preserved) {
        throw std::invalid_argument("VRS connection state update changed");
    }
    if (promotion.connection_id != connection_id ||
        promotion.previous_strength != previous_strength ||
        promotion.current_strength != current_strength) {
        throw std::invalid_argument("VRS update and promotion binding changed");
    }
}

DetachedVrsStateUpdateReceipt::DetachedVrsStateUpdateReceipt(
    std::string snapshot_id_value, std::vector<VrsConnectionStateUpdate> updates_value,
    std::array<std::string, 5> stage_order_value,
    const bool detached_state_only_value, const bool persistent_state_mutated_value,
    const bool action_authorized_value, const bool persistent_write_authorized_value,
    const bool semantic_promotion_authorized_value)
    : snapshot_id(std::move(snapshot_id_value)), updates(std::move(updates_value)),
      stage_order(std::move(stage_order_value)), detached_state_only(detached_state_only_value),
      persistent_state_mutated(persistent_state_mutated_value),
      action_authorized(action_authorized_value),
      persistent_write_authorized(persistent_write_authorized_value),
      semantic_promotion_authorized(semantic_promotion_authorized_value) {
    const std::array<std::string, 5> expected_order{
        "deja_vu", "recall", "replay", "re_evidence", "vrs_state_update"};
    if (!has_text(snapshot_id) || !detached_state_only || persistent_state_mutated ||
        action_authorized || persistent_write_authorized || semantic_promotion_authorized ||
        stage_order != expected_order) {
        throw std::invalid_argument("VRS state update authority or identity changed");
    }
    std::unordered_set<std::string> connections;
    for (const auto& update : updates) {
        if (update.promotion.snapshot_id != snapshot_id ||
            !connections.insert(update.connection_id).second) {
            throw std::invalid_argument("VRS state update authority or identity changed");
        }
    }
}

VrsExperiencePromotionDecision assess_vrs_experience_promotion(
    std::string snapshot_id, std::string connection_id,
    const double previous_strength, const double current_strength) {
    const bool was_promoted = previous_strength >= 1.0;
    const bool promoted = current_strength >= 1.0;
    return VrsExperiencePromotionDecision(
        std::move(snapshot_id), std::move(connection_id), previous_strength, current_strength,
        was_promoted && promoted ? "retain" : was_promoted ? "revoke" :
            promoted ? "promote" : "remain_unpromoted",
        promoted, promoted);
}

DetachedVrsStateUpdateReceipt plan_detached_vrs_state_update(
    std::string snapshot_id, const std::vector<VrsReplayedEpisode>& replayed,
    const std::vector<VrsReEvidenceJudgment>& judgments,
    const std::vector<std::string>& conflicting_propositions) {
    if (!has_text(snapshot_id)) throw std::invalid_argument("VRS snapshot identity is empty");
    std::unordered_map<std::string, const VrsReplayedEpisode*> episodes;
    episodes.reserve(replayed.size());
    for (const auto& episode : replayed) {
        if (!has_text(episode.episode_id) || !episodes.emplace(episode.episode_id, &episode).second) {
            throw std::invalid_argument("replayed VRS episode identity changed");
        }
    }

    std::vector<Provisional> provisional;
    provisional.reserve(judgments.size());
    for (const auto& judgment : judgments) {
        if (!has_text(judgment.episode_id) || !has_text(judgment.proposition)) {
            throw std::invalid_argument("Re-evidence judgment identity changed");
        }
        const auto episode_at = episodes.find(judgment.episode_id);
        if (episode_at == episodes.end()) {
            throw std::invalid_argument("Re-evidence judgment references absent replay episode");
        }
        const auto& episode = *episode_at->second;
        const VrsReplayObservation* edge = nullptr;
        bool canonical = false;
        for (const auto& observation : episode.observations) {
            if ((observation.edge_id && observation.vrs_strength) ||
                (observation.canonical_group_id && observation.deweighted_vrs_strength)) {
                edge = &observation;
                canonical = observation.canonical_group_id.has_value();
                break;
            }
        }
        if (edge == nullptr) continue;
        const auto previous = canonical ? *edge->deweighted_vrs_strength : *edge->vrs_strength;
        double current = previous;
        std::string action;
        if (contains(conflicting_propositions, judgment.proposition)) {
            action = "abstain_conflict";
        } else if (judgment.verdict == CurrentEvidenceVerdict::support) {
            current = previous * legacy_vrs_stable_reinforcement_factor;
            action = "reinforce";
        } else if (judgment.verdict == CurrentEvidenceVerdict::refute) {
            current = previous * legacy_vrs_unstable_weakening_factor;
            action = "weaken";
        } else {
            action = "preserve_unresolved";
        }
        const auto connection = canonical
            ? "vrs-edge-group:" + std::to_string(*edge->canonical_group_id)
            : "vrs-edge:" + std::to_string(*edge->edge_id);
        provisional.push_back({VrsConnectionStateUpdate(
            episode.episode_id, connection, judgment.proposition, judgment.verdict,
            previous, current, action,
            assess_vrs_experience_promotion(snapshot_id, connection, previous, current),
            true, {})});
    }

    std::vector<std::string> group_order;
    std::unordered_map<std::string, std::vector<const VrsConnectionStateUpdate*>> groups;
    for (const auto& row : provisional) {
        if (!groups.contains(row.update.connection_id)) group_order.push_back(row.update.connection_id);
        groups[row.update.connection_id].push_back(&row.update);
    }
    std::vector<VrsConnectionStateUpdate> unique;
    unique.reserve(group_order.size());
    for (const auto& connection : group_order) {
        const auto& rows = groups.at(connection);
        const auto& first = *rows.front();
        bool reinforce = false;
        bool weaken = false;
        bool explicit_conflict = false;
        const VrsConnectionStateUpdate* selected = &first;
        bool selected_direction = false;
        std::vector<VrsSourceJudgment> sources;
        sources.reserve(rows.size());
        for (const auto* row : rows) {
            if (row->previous_strength != first.previous_strength) {
                throw std::invalid_argument("connection aliases disagree on current snapshot strength");
            }
            reinforce = reinforce || row->update_action == "reinforce";
            weaken = weaken || row->update_action == "weaken";
            explicit_conflict = explicit_conflict || row->update_action == "abstain_conflict";
            if (!selected_direction &&
                (row->update_action == "reinforce" || row->update_action == "weaken")) {
                selected = row;
                selected_direction = true;
            }
            sources.push_back({row->episode_id, row->proposition, row->verdict});
        }
        const bool conflict = explicit_conflict || (reinforce && weaken);
        const auto current = conflict ? first.previous_strength : selected->current_strength;
        const auto action = conflict ? "abstain_conflict" : selected->update_action;
        unique.emplace_back(
            selected->episode_id, selected->connection_id, selected->proposition,
            selected->verdict, first.previous_strength, current, action,
            assess_vrs_experience_promotion(
                snapshot_id, connection, first.previous_strength, current),
            true, std::move(sources));
    }
    return DetachedVrsStateUpdateReceipt(std::move(snapshot_id), std::move(unique));
}

}  // namespace swegca::world
