#pragma once

#include "world/memory_activation.hpp"

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view resident_assimilation_source_sha256 =
    "315a652128419c5239d733a5c16de5c6da78249b3ef282825e3e5a47106e0597";

struct SealedOutcomeWave final {
    std::vector<MemoryEpisode> episodes;
    std::string sha256;
    std::size_t bytes{};
    std::vector<std::string> source_ids;
    std::vector<std::string> source_revisions;
    std::vector<std::string> source_families;
    std::function<void()> require_unchanged;

    SealedOutcomeWave(std::vector<MemoryEpisode> episodes, std::string sha256,
        std::size_t bytes, std::vector<std::string> source_ids,
        std::vector<std::string> source_revisions,
        std::vector<std::string> source_families,
        std::function<void()> require_unchanged);
};

struct PreparedResidentGeneration final {
    FullCurrentMemoryVrsSnapshot pair;
    JsonValue::Object receipt;
    std::shared_ptr<const void> runtime;
};

struct ResidentAssimilationReceipt final {
    std::string schema_version{"rozephine-paper-resident-assimilation-commit-v1"};
    std::string status{"atomic_incremental_assimilation_committed"};
    std::string previous_pair_snapshot_id;
    std::string replacement_pair_snapshot_id;
    std::string replacement_memory_snapshot_id;
    std::string replacement_vrs_snapshot_id;
    std::size_t added_distinct_source_episode_count{};
    std::map<std::string, std::size_t, std::less<>> actual_outcome_counts;
    std::string wave_sha256;
    std::size_t wave_bytes{};
    std::size_t cold_full_current_reconstruction_count_this_wave{};
    bool incremental_memory_leaf_append{true};
    bool memory_and_vrs_replaced_by_one_main_owned_cas{true};
    bool ordinary_hot_leaves_shared_by_identity{true};
    bool lookup_requires_io{};
    std::size_t commit_count_this_resident{};
    JsonValue::Object preparation;
    std::string durable_commit_marker;
    bool durable_commit_marker_ready{};
    std::optional<std::string> durable_commit_marker_error;
    bool semantic_authority{};
    bool world_authority{};
    bool action_authority{};
    bool persistent_write_authority{};
    bool model_update_authority{};
    bool distribution_authority{};
    bool p3_authority{};
};

using ResidentGenerationPreparer = std::function<PreparedResidentGeneration(
    const FullCurrentMemoryVrsSnapshot&, std::shared_ptr<const HotMemoryIndex>,
    const SealedOutcomeWave&, std::shared_ptr<const void>)>;

class DurableAssimilationMarker {
public:
    virtual ~DurableAssimilationMarker() = default;
    [[nodiscard]] virtual std::string prepare(
        const ResidentAssimilationReceipt& receipt) = 0;
    virtual void publish(std::string_view pending_marker) = 0;
};

class IncrementalResidentAssimilationController final {
public:
    IncrementalResidentAssimilationController(FullCurrentMemoryVrsSnapshot initial,
        std::size_t maximum_rows_per_wave,
        ResidentGenerationPreparer prepare_generation,
        std::shared_ptr<DurableAssimilationMarker> durable_marker,
        std::shared_ptr<const void> initial_runtime = {},
        std::size_t cold_bootstrap_count = 1);

    [[nodiscard]] FullCurrentMemoryVrsSnapshot snapshot() const;
    [[nodiscard]] std::pair<FullCurrentMemoryVrsSnapshot, std::shared_ptr<const void>>
        read_generation() const;
    [[nodiscard]] ResidentAssimilationReceipt assimilate(
        std::string_view expected_pair_snapshot_id,
        const SealedOutcomeWave& wave);
    void reconcile_pending_marker(std::string_view marker);
    [[nodiscard]] std::size_t commit_count() const;
    [[nodiscard]] std::size_t attempt_count() const;

private:
    mutable std::mutex mutex_;
    AtomicFullCurrentMemoryVrsOwner owner_;
    std::size_t maximum_rows_{};
    ResidentGenerationPreparer prepare_;
    std::shared_ptr<DurableAssimilationMarker> durable_marker_;
    std::shared_ptr<const void> runtime_;
    std::size_t commit_count_{};
    std::size_t attempt_count_{};
    std::optional<std::string> pending_marker_;
};

}  // namespace swegca::world
