#pragma once

#include "world/vrs_event_storage.hpp"
#include "world/vrs_generation_rebind.hpp"
#include "world/vrs_resident_adapter.hpp"

#include <memory>
#include <string>
#include <string_view>

namespace swegca::world {

inline constexpr std::string_view vrs_event_hot_publication_source_sha256 =
    "d799d1fc0b77b5c74a22317e0137948cd7211f8b109c05ea1c28343cefda1c15";

class BoundEventHotSource;
class DurableEventBinding;

struct EventHotPublicationReceipt final {
    std::string schema{"rozephine-event-hot-publication-v1"};
    VrsGenerationRebindReceipt rebind;
    std::string bound_parent_pair_snapshot_id;
    bool topology_changed{};
    bool ordinary_experience_enumerated{};
    bool parent_numeric_arrays_materialized{};
    bool hot_address_index_reused{true};
    bool main_pair_committed{};
    bool durable_report_verified{};
    bool cognitive_completion{};
    bool authority_granted{};
};

struct PreparedEventHotGeneration final {
    FullCurrentMemoryVrsSnapshot pair;
    std::shared_ptr<const VrsHotMemorySource> source;
    std::shared_ptr<const BoundEventSignalStorage> storage;
    std::shared_ptr<const ResidentVrsStrengthIndex> current_vrs;
    EventHotPublicationReceipt receipt;
    std::shared_ptr<const BoundEventHotSource> next_binding;
};

class BoundEventHotSource final {
public:
    [[nodiscard]] static std::shared_ptr<const BoundEventHotSource> cold_bind(
        FullCurrentMemoryVrsSnapshot pair,
        std::shared_ptr<const VrsHotMemorySource> source,
        std::shared_ptr<const BoundEventSignalStorage> storage);
    [[nodiscard]] PreparedEventHotGeneration prepare(
        const PreparedEventSignalStorage& candidate,
        std::string vrs_snapshot_id) const;

    const FullCurrentMemoryVrsSnapshot pair;
    const std::shared_ptr<const VrsHotMemorySource> source;
    const std::shared_ptr<const BoundEventSignalStorage> storage;

private:
    friend class DurableEventBinding;
    BoundEventHotSource(FullCurrentMemoryVrsSnapshot pair,
        std::shared_ptr<const VrsHotMemorySource> source,
        std::shared_ptr<const BoundEventSignalStorage> storage,
        std::shared_ptr<const VrsHotMemorySource> shape_template);
    [[nodiscard]] PreparedEventHotGeneration prepare_storage(
        std::shared_ptr<const BoundEventSignalStorage> successor,
        std::string vrs_snapshot_id) const;
    const std::shared_ptr<const VrsHotMemorySource> shape_template_;
};

}  // namespace swegca::world
