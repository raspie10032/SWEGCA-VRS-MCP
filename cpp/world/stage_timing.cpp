#include "world/stage_timing.hpp"

#include <chrono>
#include <limits>
#include <stdexcept>
#include <utility>

namespace swegca::world {
namespace {

std::uint64_t monotonic_ns() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

JsonValue integer(const std::uint64_t value) {
    if (value <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        return JsonValue(static_cast<std::int64_t>(value));
    return JsonValue(JsonInteger{std::to_string(value)});
}

}  // namespace

StageTiming::StageTiming(std::string scope, Clock clock)
    : scope_(std::move(scope)), clock_(clock ? std::move(clock) : Clock(monotonic_ns)) {
    if (scope_.empty()) throw std::invalid_argument("timing scope is required");
    started_ = last_ = clock_();
}

void StageTiming::checkpoint(std::string name) {
    if (name.empty() || stages_.contains(name))
        throw std::invalid_argument("stage names must be nonempty and unique");
    const auto now = clock_();
    if (now < last_) throw std::invalid_argument("timing clock moved backwards");
    stages_.emplace(std::move(name), now - last_);
    last_ = now;
}

JsonValue StageTiming::receipt() const {
    JsonValue::Object stages;
    for (const auto& [name, elapsed] : stages_) stages.emplace(name, integer(elapsed));
    return JsonValue::Object{
        {"schema_version", "rozephine-stage-timing-v1"},
        {"scope", scope_},
        {"clock", "monotonic_host_wall_ns"},
        {"stages_ns", JsonValue(std::move(stages))},
        {"measured_total_ns", integer(last_ - started_)},
        {"stages_are_nonoverlapping", true},
        {"nested_scopes_are_not_additive", true},
        {"device_synchronization_added", false},
        {"cognitive_authority", false},
    };
}

}  // namespace swegca::world
