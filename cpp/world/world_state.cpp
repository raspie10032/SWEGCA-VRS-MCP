#include "world/world_state.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace swegca::world {
namespace {

std::size_t element_count(const std::span<const std::uint64_t> shape) {
    std::uint64_t count = 1;
    for (const auto dimension : shape) {
        if (dimension != 0 && count > std::numeric_limits<std::uint64_t>::max() / dimension) {
            throw std::overflow_error("mask shape overflow");
        }
        count *= dimension;
    }
    if (count > std::numeric_limits<std::size_t>::max()) throw std::overflow_error("mask too large");
    return static_cast<std::size_t>(count);
}

std::size_t tensor_offset(const std::uint64_t row, const std::uint64_t slot,
                          const std::uint64_t slots, const std::uint64_t dimension) {
    return static_cast<std::size_t>((row * slots + slot) * dimension);
}

}  // namespace

void WorldConfig::validate() const {
    if (world_slots == 0 || world_dim == 0 || object_slots == 0) {
        throw std::invalid_argument("world configuration values must be positive");
    }
    if (world_slots != slot_roles.size()) throw std::invalid_argument("world slots must match frozen roles");
    if (object_slots > world_slots) throw std::invalid_argument("object slots exceed world slots");
}

BooleanMask::BooleanMask(std::vector<std::uint64_t> shape, std::vector<std::uint8_t> values)
    : shape_(std::move(shape)), values_(std::move(values)) {
    if (element_count(shape_) != values_.size()) throw std::invalid_argument("mask value count mismatch");
    if (std::any_of(values_.begin(), values_.end(), [](const auto value) { return value > 1; })) {
        throw std::invalid_argument("mask value is not boolean");
    }
}

bool BooleanMask::at(const std::uint64_t row, const std::uint64_t column) const {
    if (shape_.size() != 2 || row >= shape_[0] || column >= shape_[1]) {
        throw std::out_of_range("mask index out of range");
    }
    return values_[static_cast<std::size_t>(row * shape_[1] + column)] != 0;
}

SurfaceResidualRef SurfaceResidualRef::mark_dirty(const std::span<const std::string> regions) const {
    auto result = *this;
    for (const auto& region : regions) {
        if (std::find(result.dirty_regions.begin(), result.dirty_regions.end(), region) ==
            result.dirty_regions.end()) {
            result.dirty_regions.push_back(region);
        }
    }
    return result;
}

WorldState::WorldState(Tensor semantic_slots, BooleanMask active_mask, BooleanMask dirty_mask,
                       std::string source, std::vector<SurfaceResidualRef> surface_refs)
    : semantic_slots_(std::move(semantic_slots)), active_mask_(std::move(active_mask)),
      dirty_mask_(std::move(dirty_mask)), source_(std::move(source)),
      surface_refs_(std::move(surface_refs)) {}

void WorldState::validate(const WorldConfig& config) const {
    config.validate();
    const auto shape = semantic_slots_.shape();
    if (shape.size() != 3) throw std::invalid_argument("semantic slots must have rank 3");
    if (shape[1] != config.world_slots || shape[2] != config.world_dim) {
        throw std::invalid_argument("semantic slot shape mismatch");
    }
    const std::array<std::uint64_t, 2> expected{shape[0], config.world_slots};
    if (!std::equal(active_mask_.shape().begin(), active_mask_.shape().end(),
                    expected.begin(), expected.end()) ||
        !std::equal(dirty_mask_.shape().begin(), dirty_mask_.shape().end(),
                    expected.begin(), expected.end())) {
        throw std::invalid_argument("world mask shape mismatch");
    }
}

bool WorldState::exact_equal(const WorldState& other) const noexcept {
    return semantic_slots_.exact_equal(other.semantic_slots_) && active_mask_ == other.active_mask_ &&
        dirty_mask_ == other.dirty_mask_ && source_ == other.source_ && surface_refs_ == other.surface_refs_;
}

WorldState edit_world_slots(const WorldState& state, const std::span<const std::uint64_t> slot_indices,
                            const Tensor& updates, const WorldConfig& config,
                            const std::span<const std::string> dirty_regions) {
    state.validate(config);
    if (slot_indices.empty()) throw std::invalid_argument("at least one slot must be edited");
    for (const auto slot : slot_indices) {
        if (slot >= config.world_slots) throw std::out_of_range("slot index outside world workspace");
    }
    const auto state_shape = state.semantic_slots().shape();
    const auto update_shape = updates.shape();
    if (update_shape.size() != 3 || update_shape[0] != state_shape[0] ||
        update_shape[1] != slot_indices.size() || update_shape[2] != config.world_dim ||
        updates.dtype() != state.semantic_slots().dtype() ||
        updates.device() != state.semantic_slots().device()) {
        throw std::invalid_argument("world edit update tensor mismatch");
    }
    std::vector<double> values(state.semantic_slots().values().begin(), state.semantic_slots().values().end());
    for (std::uint64_t batch = 0; batch != state_shape[0]; ++batch) {
        for (std::size_t update = 0; update != slot_indices.size(); ++update) {
            const auto target = tensor_offset(batch, slot_indices[update], config.world_slots, config.world_dim);
            const auto source = tensor_offset(batch, update, slot_indices.size(), config.world_dim);
            std::copy_n(updates.values().begin() + static_cast<std::ptrdiff_t>(source),
                        static_cast<std::size_t>(config.world_dim),
                        values.begin() + static_cast<std::ptrdiff_t>(target));
        }
    }
    auto dirty_values = std::vector<std::uint8_t>(state.dirty_mask().values().begin(),
                                                   state.dirty_mask().values().end());
    for (std::uint64_t batch = 0; batch != state_shape[0]; ++batch) {
        for (const auto slot : slot_indices) {
            dirty_values[static_cast<std::size_t>(batch * config.world_slots + slot)] = 1;
        }
    }
    std::vector<SurfaceResidualRef> refs;
    refs.reserve(state.surface_refs().size());
    for (const auto& reference : state.surface_refs()) refs.push_back(reference.mark_dirty(dirty_regions));
    WorldState result(
        Tensor(state.semantic_slots().dtype(),
               std::vector<std::uint64_t>(state_shape.begin(), state_shape.end()), std::move(values),
               std::string(state.semantic_slots().device())),
        state.active_mask().clone(), BooleanMask({state_shape[0], config.world_slots}, std::move(dirty_values)),
        std::string(state.source()) + ":edited", std::move(refs));
    result.validate(config);
    return result;
}

WorldState merge_persistent_scene_memory(const WorldState& previous, const WorldState& observation,
                                         const BooleanMask& visible_object_mask,
                                         const WorldConfig& config) {
    previous.validate(config);
    observation.validate(config);
    if (!std::equal(previous.semantic_slots().shape().begin(), previous.semantic_slots().shape().end(),
                    observation.semantic_slots().shape().begin(), observation.semantic_slots().shape().end()) ||
        previous.semantic_slots().dtype() != observation.semantic_slots().dtype() ||
        previous.semantic_slots().device() != observation.semantic_slots().device()) {
        throw std::invalid_argument("previous and observed world states do not align");
    }
    const auto shape = previous.semantic_slots().shape();
    const std::array<std::uint64_t, 2> expected{shape[0], config.object_slots};
    if (!std::equal(visible_object_mask.shape().begin(), visible_object_mask.shape().end(),
                    expected.begin(), expected.end())) {
        throw std::invalid_argument("visible object mask shape mismatch");
    }
    std::vector<double> merged(observation.semantic_slots().values().begin(),
                               observation.semantic_slots().values().end());
    for (std::uint64_t batch = 0; batch != shape[0]; ++batch) {
        for (std::uint64_t slot = 0; slot != config.object_slots; ++slot) {
            if (visible_object_mask.at(batch, slot)) continue;
            const auto offset = tensor_offset(batch, slot, config.world_slots, config.world_dim);
            std::copy_n(previous.semantic_slots().values().begin() + static_cast<std::ptrdiff_t>(offset),
                        static_cast<std::size_t>(config.world_dim),
                        merged.begin() + static_cast<std::ptrdiff_t>(offset));
        }
    }
    std::vector<std::uint8_t> active;
    active.reserve(previous.active_mask().values().size());
    for (std::size_t index = 0; index != previous.active_mask().values().size(); ++index) {
        active.push_back(static_cast<std::uint8_t>(previous.active_mask().values()[index] != 0 ||
                                                  observation.active_mask().values()[index] != 0));
    }
    std::vector<SurfaceResidualRef> refs(
        observation.surface_refs().empty() ? previous.surface_refs().begin() : observation.surface_refs().begin(),
        observation.surface_refs().empty() ? previous.surface_refs().end() : observation.surface_refs().end());
    WorldState result(
        Tensor(observation.semantic_slots().dtype(),
               std::vector<std::uint64_t>(shape.begin(), shape.end()), std::move(merged),
               std::string(observation.semantic_slots().device())),
        BooleanMask({shape[0], config.world_slots}, std::move(active)), observation.dirty_mask().clone(),
        "persistent:" + std::string(observation.source()), std::move(refs));
    result.validate(config);
    return result;
}

}  // namespace swegca::world
