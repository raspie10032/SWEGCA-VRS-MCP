#include "world/hot_memory_step.hpp"

namespace swegca::world {

MemoryStep make_hot_memory_step(
    std::string phase, JsonValue::Object observation,
    std::vector<std::string> relations, std::string judgment,
    std::string outcome, std::vector<std::string> evidence_refs) {
    // JsonValue owns every nested array/object and therefore already provides
    // the detached immutable value graph required by the Python hot adapter.
    return MemoryStep(std::move(phase), std::move(observation),
                      std::move(relations), std::move(judgment),
                      std::move(outcome), std::move(evidence_refs));
}

}  // namespace swegca::world
