#include "world/world_state.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace swegca::world;

namespace {

WorldState state(const WorldConfig& config, const double base, std::string source,
                 const bool with_surface = true) {
    std::vector<double> values(static_cast<std::size_t>(config.world_slots * config.world_dim));
    for (std::size_t index = 0; index != values.size(); ++index) values[index] = base + index;
    std::vector<std::uint8_t> active(config.world_slots, 1);
    std::vector<std::uint8_t> dirty(config.world_slots, 0);
    std::vector<SurfaceResidualRef> refs;
    if (with_surface) refs.push_back({"image", "test://surface", {1, 4, 8, 8}, {}});
    return WorldState(Tensor(TensorDType::float32, {1, config.world_slots, config.world_dim},
                             std::move(values)),
                      BooleanMask({1, config.world_slots}, std::move(active)),
                      BooleanMask({1, config.world_slots}, std::move(dirty)),
                      std::move(source), std::move(refs));
}

template<class Function> void rejects(Function&& function) {
    bool rejected = false;
    try { function(); } catch (const std::exception&) { rejected = true; }
    assert(rejected);
}

}  // namespace

int main() {
    WorldConfig config;
    config.world_dim = 4;
    config.validate();
    assert(slot_roles[0] == "object_0" && slot_roles[30] == "verification" &&
           slot_roles[31] == "global");

    const auto original = state(config, 0.0, "test");
    original.validate(config);
    const std::vector<std::uint64_t> selected{2, 22};
    std::vector<double> changed;
    for (const auto slot : selected) {
        const auto begin = original.semantic_slots().values().begin() +
            static_cast<std::ptrdiff_t>(slot * config.world_dim);
        for (std::uint64_t value = 0; value != config.world_dim; ++value) {
            changed.push_back(*(begin + static_cast<std::ptrdiff_t>(value)) + 1.0);
        }
    }
    const Tensor updates(TensorDType::float32, {1, selected.size(), config.world_dim}, changed);
    const std::vector<std::string> dirty_regions{"object:2", "lighting", "object:2"};
    const auto edited = edit_world_slots(original, selected, updates, config, dirty_regions);
    assert(original.source() == "test" && edited.source() == "test:edited");
    assert(!original.dirty_mask().at(0, 2) && edited.dirty_mask().at(0, 2));
    assert(!original.dirty_mask().at(0, 22) && edited.dirty_mask().at(0, 22));
    assert(edited.surface_refs()[0].dirty_regions ==
           (std::vector<std::string>{"object:2", "lighting"}));
    for (std::uint64_t slot = 0; slot != config.world_slots; ++slot) {
        for (std::uint64_t element = 0; element != config.world_dim; ++element) {
            const auto index = static_cast<std::size_t>(slot * config.world_dim + element);
            if (slot == 2 || slot == 22) {
                assert(edited.semantic_slots().values()[index] ==
                       original.semantic_slots().values()[index] + 1.0);
            } else {
                assert(edited.semantic_slots().values()[index] == original.semantic_slots().values()[index]);
            }
        }
    }

    const auto previous = state(config, 1000.0, "previous");
    const auto observation = state(config, 2000.0, "observed", false);
    std::vector<std::uint8_t> visible(config.object_slots, 0);
    visible[0] = 1;
    const auto merged = merge_persistent_scene_memory(
        previous, observation, BooleanMask({1, config.object_slots}, std::move(visible)), config);
    assert(merged.source() == "persistent:observed");
    assert(merged.surface_refs().size() == previous.surface_refs().size());
    assert(std::equal(merged.surface_refs().begin(), merged.surface_refs().end(),
                      previous.surface_refs().begin(), previous.surface_refs().end()));
    for (std::uint64_t slot = 0; slot != config.object_slots; ++slot) {
        const auto expected = slot == 0 ? observation.semantic_slots().values() : previous.semantic_slots().values();
        for (std::uint64_t element = 0; element != config.world_dim; ++element) {
            const auto index = static_cast<std::size_t>(slot * config.world_dim + element);
            assert(merged.semantic_slots().values()[index] == expected[index]);
        }
    }
    for (std::uint64_t slot = config.object_slots; slot != config.world_slots; ++slot) {
        const auto index = static_cast<std::size_t>(slot * config.world_dim);
        assert(merged.semantic_slots().values()[index] == observation.semantic_slots().values()[index]);
    }

    rejects([&] { WorldConfig bad = config; bad.world_slots = 31; bad.validate(); });
    rejects([&] { original.validate(WorldConfig{32, 256, 8}); });
    rejects([&] { (void)edit_world_slots(original, {}, updates, config); });
    rejects([&] {
        const std::vector<std::uint64_t> bad{32};
        (void)edit_world_slots(original, bad, updates, config);
    });
    rejects([&] {
        (void)merge_persistent_scene_memory(previous, observation, BooleanMask({1, 7},
            std::vector<std::uint8_t>(7, 0)), config);
    });
    std::cout << "world state/edit/scene memory tests passed\n";
}
