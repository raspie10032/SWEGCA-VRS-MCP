#pragma once

#include "world/vrs_memory_bridge.hpp"
#include "world/ordinary_source_router.hpp"

#include <array>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace swegca::world {

struct CausalEvaluationReceipt;

inline constexpr std::string_view vrs_generation_rebind_source_sha256 =
    "df66a5553ada1fa6b6ebf732f61e82c00a7d82b10bfa01b753edc350a991878b";
inline constexpr std::array<std::string_view, 4> vrs_virtual_episode_prefixes{
    "vrs-edge:", "vrs-edge-group:", "vrs-term:", "vrs-evidence-request:"};

class VrsGenerationBoundMemoryIndex final : public HotMemoryIndex {
public:
    VrsGenerationBoundMemoryIndex(
        std::vector<std::shared_ptr<const HotMemoryIndex>> ordinary_sources,
        std::shared_ptr<const VrsHotMemorySource> vrs_source,
        std::string effective_vrs_snapshot_id,
        std::vector<std::string> replaced_vrs_source_snapshot_ids);

    [[nodiscard]] std::string_view snapshot_id() const noexcept override;
    [[nodiscard]] std::size_t episode_count() const noexcept override;
    [[nodiscard]] const std::map<std::string, std::size_t, std::less<>>&
        outcome_counts() const noexcept override;
    [[nodiscard]] const MemoryEpisode& episode(std::string_view episode_id) const override;
    [[nodiscard]] bool contains_episode(std::string_view episode_id) const override;
    [[nodiscard]] std::vector<std::string> episode_ids_for_cue(
        std::string_view cue) const override;
    [[nodiscard]] std::vector<std::string> iter_episode_ids() const override;

    const std::vector<std::shared_ptr<const HotMemoryIndex>> ordinary_sources;
    const std::shared_ptr<const VrsHotMemorySource> vrs_source;
    const std::string effective_vrs_snapshot_id;
    const std::vector<std::string> replaced_vrs_source_snapshot_ids;

private:
    std::string snapshot_id_;
    std::size_t episode_count_{};
    std::map<std::string, std::size_t, std::less<>> outcome_counts_;
    std::unique_ptr<OrdinarySourceRouter> ordinary_router_;
};

struct VrsGenerationRebindReceipt final {
    std::string schema_version{"rozephine-vrs-generation-rebind-receipt-v1"};
    std::string previous_pair_snapshot_id;
    std::string replacement_pair_snapshot_id;
    std::string previous_memory_snapshot_id;
    std::string replacement_memory_snapshot_id;
    std::string effective_vrs_snapshot_id;
    std::vector<std::string> replaced_vrs_source_snapshot_ids;
    std::string replacement_vrs_source_snapshot_id;
    std::size_t ordinary_source_count{};
    bool ordinary_sources_shared_by_identity{};
    bool old_vrs_source_retained_in_replacement{};
    std::size_t full_experience_enumeration_count{};
    std::size_t persistent_state_mutation_count{};
    bool semantic_authority{};
    bool world_authority{};
    bool action_authority{};
    bool persistent_write_authority{};
    bool model_update_authority{};
    bool distribution_authority{};
    bool p3_authority{};
    bool atom_sidecar_rebound{};
    std::size_t expired_virtual_sidecar_parent_count{};
    bool ordinary_atom_sidecars_shared{};
    bool premise_index_rebound{};
    std::size_t expired_virtual_premise_source_count{};
    bool ordinary_premise_objects_shared{};
};

struct ReboundFullCurrentVrs final {
    FullCurrentMemoryVrsSnapshot pair;
    VrsGenerationRebindReceipt receipt;
};

[[nodiscard]] std::vector<std::shared_ptr<const HotMemoryIndex>>
vrs_leaf_sources(const std::shared_ptr<const HotMemoryIndex>& index);
[[nodiscard]] ReboundFullCurrentVrs rebind_full_current_vrs_source(
    const FullCurrentMemoryVrsSnapshot& pair,
    std::shared_ptr<const VrsHotMemorySource> replacement,
    std::string replacement_vrs_report_sha256);

struct BlindPilotUnitReceipt final {
    std::size_t request_sequence{};
    std::vector<std::string> recalled_episode_ids;
    std::string current_decision;
    std::string frozen_decision;
    std::string no_vrs_decision;
};

struct BlindPilotGateReceipt final {
    std::string schema_version{"rozephine-paper-blind-pilot-gate-v1"};
    std::string status{"eligible_for_preregistered_approximately_k100_ablation"};
    std::size_t completed_unit_count{};
    std::string pair_snapshot_id;
    std::string memory_snapshot_id;
    bool all_arms_identical_recall{true};
    bool current_differs_from_frozen_and_no_vrs{true};
    bool no_vrs_abstained_without_decision_callback{true};
    std::size_t full_current_rebuilds_per_request{};
    bool growth_claimed{};
    std::vector<BlindPilotUnitReceipt> unit_receipts;
    bool semantic_authority{};
    bool world_authority{};
    bool action_authority{};
    bool persistent_write_authority{};
    bool model_update_authority{};
    bool distribution_authority{};
    bool p3_authority{};
};

[[nodiscard]] BlindPilotGateReceipt validate_blind_pilot_gate(
    const std::vector<CausalEvaluationReceipt>& receipts,
    std::size_t minimum_units = 3);

}  // namespace swegca::world
