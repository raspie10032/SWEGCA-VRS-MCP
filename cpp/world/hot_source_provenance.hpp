#pragma once

#include "world/crossing_provenance.hpp"
#include "world/paper_hot_causal_ablation.hpp"

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view hot_source_provenance_source_sha256 =
    "138fd702aa4508cf34b411ff7f85f52afe97837db1df4f1b128fc22b28266b5a";

struct SourceBinding final {
    std::string episode_id;
    std::string revision;
    std::vector<std::string> source_addresses;
    std::string source_item_id;
    std::string outcome;
    friend bool operator==(const SourceBinding&, const SourceBinding&) = default;
};

struct RelationSources final {
    std::string episode_id;
    std::vector<SourceBinding> sources;
    std::optional<std::string> unresolved_reason;
};

struct SourceDecision final {
    std::string memory_snapshot_id;
    std::string vrs_snapshot_id;
    std::string decision;
    std::string reason;
    std::vector<RelationSources> contributors;
    std::vector<std::pair<std::string, std::string>> rejected_replays;
    bool source_count_is_causal_effect{};
};

class HotSourceProvenance final {
public:
    using Binding = std::pair<RelationIdentity, std::vector<SourceBinding>>;
    using Bindings = std::map<std::size_t, Binding>;

    [[nodiscard]] static std::shared_ptr<const HotSourceProvenance> build(
        std::shared_ptr<const HotMemoryIndex> memory,
        std::string vrs_snapshot_id,
        const std::vector<RelationSummaryRow>& relation_summary,
        const std::vector<RelationEvidenceRow>& relation_evidence);

    [[nodiscard]] RelationSources resolve(const ReplayedEpisode& replay) const;
    [[nodiscard]] SourceDecision explain(const HotMemoryIndex& memory,
        const MemoryActivationReceipt& activation) const;
    [[nodiscard]] std::string decide(const HotMemoryIndex& memory,
        const MemoryActivationReceipt& activation) const;

    const std::shared_ptr<const HotMemoryIndex> memory;
    const std::string vrs_snapshot_id;
    const Bindings bindings;

private:
    HotSourceProvenance(std::shared_ptr<const HotMemoryIndex> memory,
        std::string vrs_snapshot_id, Bindings bindings);
};

class ProvenanceEpisodeRoles final : public HotEpisodeRoleIndex {
public:
    ProvenanceEpisodeRoles(std::shared_ptr<const HotSourceProvenance> provenance,
        std::set<std::string, std::less<>> repair_source_ids);
    [[nodiscard]] EpisodeRole role(std::string_view episode_id) const override;
private:
    std::shared_ptr<const HotSourceProvenance> provenance_;
    std::set<std::string, std::less<>> repair_source_ids_;
};

}  // namespace swegca::world
