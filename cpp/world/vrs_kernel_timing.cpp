#include "world/vrs_kernel_timing.hpp"

#include <chrono>
#include <limits>
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

JsonValue object_of(const std::map<std::string, std::uint64_t, std::less<>>& values) {
    JsonValue::Object result;
    for (const auto& [key, value] : values) result.emplace(key, integer(value));
    return JsonValue(std::move(result));
}

}  // namespace

VrsKernelTiming::VrsKernelTiming(std::string device) : device_(std::move(device)) {
    started_ = last_ = monotonic_ns();
}

void VrsKernelTiming::checkpoint(std::string name) {
    const auto now = monotonic_ns();
    stages_[name] += now - last_;
    counts_[name] += 1;
    last_ = now;
}

void VrsKernelTiming::finish(JsonValue::Object& output, const std::uint64_t nodes,
                             const std::uint64_t edges, const std::uint64_t cycles,
                             const std::uint64_t passes,
                             const std::uint64_t batch_size) const {
    output.insert_or_assign("schema_version", "vrs-reference-kernel-timing-v1");
    output.insert_or_assign("status", "completed_numerical_reference_not_cognitive_completion");
    output.insert_or_assign("clock", "monotonic_host_wall_ns");
    output.insert_or_assign("device", device_);
    output.insert_or_assign("device_synchronization_added", false);
    output.insert_or_assign("device_execution_time_claimed", false);
    output.insert_or_assign("stages_ns", object_of(stages_));
    output.insert_or_assign("stage_calls", object_of(counts_));
    output.insert_or_assign("measured_total_ns", integer(last_ - started_));
    output.insert_or_assign("stages_are_nonoverlapping", true);
    output.insert_or_assign("nested_scopes_are_not_additive", true);
    output.insert_or_assign("measurement_overhead_included", true);
    output.insert_or_assign("node_count", integer(nodes));
    output.insert_or_assign("edge_count", integer(edges));
    output.insert_or_assign("shuffle_cycles", integer(cycles));
    output.insert_or_assign("reinforcement_passes", integer(passes));
    output.insert_or_assign("edge_batch_size", integer(batch_size));
    output.insert_or_assign("full_edge_evaluations", integer(edges * cycles * passes));
    output.insert_or_assign("cognitive_authority", false);
}

}  // namespace swegca::world
