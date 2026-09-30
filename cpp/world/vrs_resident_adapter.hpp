#pragma once

#include "world/paper_hot_causal_ablation.hpp"
#include "world/vrs_memory_bridge.hpp"

#include <cstddef>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view vrs_resident_adapter_source_sha256 =
    "a0f600abf7773988453a855d8e6cff1e470e0d7f84b391ffcc86daaf16616010";

class ResidentVrsStrengthIndex final : public HotVrsStrengthIndex {
public:
    ResidentVrsStrengthIndex(std::shared_ptr<const VrsHotMemorySource> source,
        std::shared_ptr<const std::vector<double>> strengths,
        std::string snapshot_id);
    [[nodiscard]] static std::shared_ptr<const ResidentVrsStrengthIndex> current(
        std::shared_ptr<const VrsHotMemorySource> source,
        std::string snapshot_id);
    [[nodiscard]] std::string_view snapshot_id() const noexcept override;
    [[nodiscard]] double strength(std::string_view episode_id) const noexcept override;

    const std::shared_ptr<const VrsHotMemorySource> source;
    const std::shared_ptr<const std::vector<double>> strengths;
private:
    std::string snapshot_id_;
};

class ResidentEpisodeRoleIndex final : public HotEpisodeRoleIndex {
public:
    ResidentEpisodeRoleIndex(std::shared_ptr<const VrsHotMemorySource> source,
        std::set<std::size_t> repair_source_term_ids = {},
        std::set<std::string, std::less<>> repair_episode_ids = {},
        EpisodeRole default_role = EpisodeRole::base,
        bool codex_or_evaluator_allowlist_used = false);
    [[nodiscard]] EpisodeRole role(std::string_view episode_id) const noexcept override;

private:
    std::shared_ptr<const VrsHotMemorySource> source_;
    std::set<std::size_t> repair_source_term_ids_;
    std::set<std::string, std::less<>> repair_episode_ids_;
    EpisodeRole default_role_{};
};

}  // namespace swegca::world
