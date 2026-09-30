#include "world/vrs_resident_adapter.hpp"

#include <charconv>
#include <optional>
#include <stdexcept>

namespace swegca::world {
namespace {

bool digest_id(const std::string_view value) {
    if (value.size() != 64) return false;
    for (const auto c : value)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

std::optional<std::size_t> number(const std::string_view value) noexcept {
    std::size_t result{};
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size()) return std::nullopt;
    return result;
}

std::optional<std::size_t> edge_group(
    const VrsHotMemorySource& source, const std::string_view episode_id) noexcept {
    if (episode_id.starts_with("vrs-edge-group:")) {
        const auto canonical = dynamic_cast<const CanonicalVrsHotMemorySource*>(&source);
        if (!canonical) return std::nullopt;
        const auto group = number(episode_id.substr(std::string_view("vrs-edge-group:").size()));
        if (!group || *group >= canonical->edge_source.size()) return std::nullopt;
        return group;
    }
    if (!episode_id.starts_with("vrs-edge:")) return std::nullopt;
    const auto edge = number(episode_id.substr(std::string_view("vrs-edge:").size()));
    if (!edge) return std::nullopt;
    if (const auto canonical = dynamic_cast<const CanonicalVrsHotMemorySource*>(&source)) {
        if (*edge >= canonical->original_edge_count()) return std::nullopt;
        try { return canonical->canonical_group_for_member(*edge); }
        catch (...) { return std::nullopt; }
    }
    return *edge < source.edge_source.size() ? edge : std::nullopt;
}

}  // namespace

ResidentVrsStrengthIndex::ResidentVrsStrengthIndex(
    std::shared_ptr<const VrsHotMemorySource> source_value,
    std::shared_ptr<const std::vector<double>> strengths_value,
    std::string snapshot)
    : source(std::move(source_value)), strengths(std::move(strengths_value)),
      snapshot_id_(std::move(snapshot)) {
    if (!source || !strengths)
        throw std::invalid_argument("resident VRS source and strengths required");
    if (strengths->size() > source->vrs_strength.size())
        throw std::invalid_argument("resident VRS strength generation exceeds source graph");
    if (!digest_id(snapshot_id_))
        throw std::invalid_argument("VRS snapshot must be a SHA-256 digest");
}

std::shared_ptr<const ResidentVrsStrengthIndex> ResidentVrsStrengthIndex::current(
    std::shared_ptr<const VrsHotMemorySource> source, std::string snapshot_id) {
    if (!source) throw std::invalid_argument("resident VRS source required");
    auto strengths = std::shared_ptr<const std::vector<double>>(source, &source->vrs_strength);
    return std::make_shared<const ResidentVrsStrengthIndex>(
        std::move(source), std::move(strengths), std::move(snapshot_id));
}

std::string_view ResidentVrsStrengthIndex::snapshot_id() const noexcept { return snapshot_id_; }

double ResidentVrsStrengthIndex::strength(const std::string_view episode_id) const noexcept {
    const auto group = edge_group(*source, episode_id);
    return !group || *group >= strengths->size() ? 0.0 : (*strengths)[*group];
}

ResidentEpisodeRoleIndex::ResidentEpisodeRoleIndex(
    std::shared_ptr<const VrsHotMemorySource> source,
    std::set<std::size_t> repair_terms,
    std::set<std::string, std::less<>> repair_episodes,
    const EpisodeRole default_role, const bool allowlist)
    : source_(std::move(source)), repair_source_term_ids_(std::move(repair_terms)),
      repair_episode_ids_(std::move(repair_episodes)), default_role_(default_role) {
    if (!source_) throw std::invalid_argument("resident VRS source required");
    if (allowlist) throw std::invalid_argument("episode role metadata became a retrieval allowlist");
    for (const auto term : repair_source_term_ids_)
        if (term >= source_->terms.size())
            throw std::invalid_argument("repair source-term provenance changed");
    for (const auto& episode : repair_episode_ids_)
        if (episode.empty()) throw std::invalid_argument("repair episode provenance changed");
}

EpisodeRole ResidentEpisodeRoleIndex::role(const std::string_view episode_id) const noexcept {
    if (const auto group = edge_group(*source_, episode_id)) {
        const auto term = static_cast<std::size_t>(source_->edge_source[*group]);
        return repair_source_term_ids_.contains(term) ? EpisodeRole::repair : EpisodeRole::base;
    }
    if (repair_episode_ids_.contains(episode_id)) return EpisodeRole::repair;
    if (episode_id.starts_with("vrs-term:") || episode_id.starts_with("vrs-evidence-request:"))
        return EpisodeRole::other;
    return default_role_;
}

}  // namespace swegca::world
