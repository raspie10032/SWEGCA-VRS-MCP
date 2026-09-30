#pragma once

#include "world/lossless_blocks.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view temporal_axis_concept_source_sha256 =
    "26422922a88ef0b5bc07088e675c4b07d501fde3a2cfa79ef84f6d7b04d409c3";
inline constexpr std::int64_t temporal_ns_per_second = 1'000'000'000;
inline constexpr std::int64_t temporal_default_rate = 192'000;

struct TimePoint final {
    std::string clock;
    std::int64_t ns{};
    std::int64_t uncertainty_ns{};
    TimePoint(std::string clock, std::int64_t ns, std::int64_t uncertainty_ns = 0);
    friend bool operator==(const TimePoint&, const TimePoint&) = default;
};

[[nodiscard]] std::pair<std::int64_t, std::int64_t> split_tick(
    std::int64_t ns, std::int64_t rate = temporal_default_rate);
[[nodiscard]] std::int64_t join_tick(
    std::int64_t tick, std::int64_t residual,
    std::int64_t rate = temporal_default_rate);
[[nodiscard]] std::optional<std::pair<std::int64_t, std::int64_t>> elapsed_bounds(
    const std::optional<TimePoint>& first, const std::optional<TimePoint>& second);
[[nodiscard]] std::string_view temporal_relation(
    const std::optional<TimePoint>& first, const std::optional<TimePoint>& second);

struct TemporalEvent final {
    std::string event_id;
    std::optional<TimePoint> occurred;
    TimePoint observed;
    std::string source;
    std::string kind;
    std::string outcome;
    std::vector<std::byte> payload;
    TemporalEvent(std::string event_id, std::optional<TimePoint> occurred,
                  TimePoint observed, std::string source, std::string kind,
                  std::string outcome, std::vector<std::byte> payload);
};

enum class TimeAxisMode : std::uint8_t { nanoseconds, ticks, ticks_residual };

struct TimeAxisBlock final {
    std::int64_t first{};
    std::int64_t last{};
    std::size_t count{};
    std::shared_ptr<const LosslessBlob> blob;
    [[nodiscard]] std::vector<std::int64_t> values(
        TimeAxisMode mode, std::int64_t rate) const;
};

class PackedTimeAxis final {
public:
    PackedTimeAxis(std::string clock, TimeAxisMode mode, std::int64_t rate,
                   std::vector<TimeAxisBlock> chunks);
    [[nodiscard]] static PackedTimeAxis build(
        std::span<const std::int64_t> times, std::string clock,
        TimeAxisMode mode = TimeAxisMode::ticks_residual,
        std::size_t chunk_rows = 128,
        std::int64_t rate = temporal_default_rate);
    [[nodiscard]] std::vector<std::int64_t> read_range(
        std::int64_t lower, std::int64_t upper) const;
    [[nodiscard]] PackedTimeAxis append(
        std::span<const std::int64_t> times, std::string_view clock) const;
    [[nodiscard]] std::size_t payload_bytes() const noexcept;
    [[nodiscard]] std::size_t raw_coordinate_bytes() const noexcept;

    const std::string clock;
    const TimeAxisMode mode;
    const std::int64_t rate;
    const std::vector<TimeAxisBlock> chunks;
    const std::vector<std::int64_t> ends;
};

}  // namespace swegca::world
