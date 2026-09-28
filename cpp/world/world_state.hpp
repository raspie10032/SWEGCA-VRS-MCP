#pragma once

#include "world/cognitive_state.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace swegca::world {

inline constexpr std::array<std::string_view, 32> slot_roles{
    "object_0", "object_1", "object_2", "object_3", "object_4", "object_5", "object_6", "object_7",
    "relation_0", "relation_1", "relation_2", "relation_3", "relation_4", "relation_5", "relation_6", "relation_7",
    "action_0", "action_1", "action_2", "action_3", "camera_0", "camera_1", "lighting_0", "lighting_1",
    "environment_0", "environment_1", "audio_event_0", "audio_event_1", "narrative", "constraints",
    "verification", "global"};

struct WorldConfig final {
    std::uint64_t world_slots{32};
    std::uint64_t world_dim{256};
    std::uint64_t object_slots{8};
    void validate() const;
};

class BooleanMask final {
public:
    BooleanMask(std::vector<std::uint64_t> shape, std::vector<std::uint8_t> values);
    [[nodiscard]] std::span<const std::uint64_t> shape() const noexcept { return shape_; }
    [[nodiscard]] std::span<const std::uint8_t> values() const noexcept { return values_; }
    [[nodiscard]] bool at(std::uint64_t row, std::uint64_t column) const;
    [[nodiscard]] BooleanMask clone() const { return *this; }
    friend bool operator==(const BooleanMask&, const BooleanMask&) = default;

private:
    std::vector<std::uint64_t> shape_;
    std::vector<std::uint8_t> values_;
};

struct SurfaceResidualRef final {
    std::string modality;
    std::string storage_key;
    std::vector<std::uint64_t> shape;
    std::vector<std::string> dirty_regions;

    [[nodiscard]] SurfaceResidualRef mark_dirty(std::span<const std::string> regions) const;
    friend bool operator==(const SurfaceResidualRef&, const SurfaceResidualRef&) = default;
};

class WorldState final {
public:
    WorldState(Tensor semantic_slots, BooleanMask active_mask, BooleanMask dirty_mask,
               std::string source, std::vector<SurfaceResidualRef> surface_refs = {});

    [[nodiscard]] const Tensor& semantic_slots() const noexcept { return semantic_slots_; }
    [[nodiscard]] const BooleanMask& active_mask() const noexcept { return active_mask_; }
    [[nodiscard]] const BooleanMask& dirty_mask() const noexcept { return dirty_mask_; }
    [[nodiscard]] std::string_view source() const noexcept { return source_; }
    [[nodiscard]] std::span<const SurfaceResidualRef> surface_refs() const noexcept { return surface_refs_; }
    void validate(const WorldConfig& config) const;
    [[nodiscard]] bool exact_equal(const WorldState& other) const noexcept;

private:
    Tensor semantic_slots_;
    BooleanMask active_mask_;
    BooleanMask dirty_mask_;
    std::string source_;
    std::vector<SurfaceResidualRef> surface_refs_;
};

[[nodiscard]] WorldState edit_world_slots(const WorldState& state,
    std::span<const std::uint64_t> slot_indices, const Tensor& updates,
    const WorldConfig& config, std::span<const std::string> dirty_regions = {});

[[nodiscard]] WorldState merge_persistent_scene_memory(const WorldState& previous,
    const WorldState& observation, const BooleanMask& visible_object_mask,
    const WorldConfig& config);

}  // namespace swegca::world
