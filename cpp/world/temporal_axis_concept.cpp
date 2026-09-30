#include "world/temporal_axis_concept.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace swegca::world {
namespace {

bool nonblank(const std::string_view value) {
    return std::ranges::any_of(value, [](const unsigned char c) { return c > 0x20U; });
}

void put_u64(std::vector<std::byte>& out, const std::uint64_t value) {
    for (unsigned shift = 0; shift != 64; shift += 8)
        out.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
}
void put_u32(std::vector<std::byte>& out, const std::uint32_t value) {
    for (unsigned shift = 0; shift != 32; shift += 8)
        out.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
}
std::uint64_t get_u64(const std::span<const std::byte> data, const std::size_t at) {
    std::uint64_t value = 0;
    for (unsigned shift = 0; shift != 64; shift += 8)
        value |= static_cast<std::uint64_t>(std::to_integer<unsigned>(data[at + shift / 8])) << shift;
    return value;
}
std::uint32_t get_u32(const std::span<const std::byte> data, const std::size_t at) {
    std::uint32_t value = 0;
    for (unsigned shift = 0; shift != 32; shift += 8)
        value |= static_cast<std::uint32_t>(std::to_integer<unsigned>(data[at + shift / 8])) << shift;
    return value;
}

}  // namespace

TimePoint::TimePoint(std::string clock_value, const std::int64_t ns_value,
                     const std::int64_t uncertainty)
    : clock(std::move(clock_value)), ns(ns_value), uncertainty_ns(uncertainty) {
    if (!nonblank(clock)) throw std::invalid_argument("explicit clock domain required");
    if (uncertainty_ns < 0) throw std::invalid_argument("negative uncertainty");
}

std::pair<std::int64_t, std::int64_t> split_tick(
    const std::int64_t ns, const std::int64_t rate) {
    if (rate <= 0) throw std::invalid_argument("positive rate required");
    const __int128 value = static_cast<__int128>(ns) * rate;
    auto quotient = value / temporal_ns_per_second;
    auto remainder = value % temporal_ns_per_second;
    if (remainder < 0) { remainder += temporal_ns_per_second; --quotient; }
    if (quotient < std::numeric_limits<std::int64_t>::min() ||
        quotient > std::numeric_limits<std::int64_t>::max())
        throw std::overflow_error("tick coordinate overflow");
    return {static_cast<std::int64_t>(quotient), static_cast<std::int64_t>(remainder)};
}

std::int64_t join_tick(const std::int64_t tick, const std::int64_t residual,
                       const std::int64_t rate) {
    if (rate <= 0 || residual < 0 || residual >= temporal_ns_per_second)
        throw std::invalid_argument("invalid rate or residual");
    const __int128 value = static_cast<__int128>(tick) * temporal_ns_per_second + residual;
    if (value % rate) throw std::invalid_argument("coordinate is not an exact integer nanosecond");
    const auto result = value / rate;
    if (result < std::numeric_limits<std::int64_t>::min() ||
        result > std::numeric_limits<std::int64_t>::max())
        throw std::overflow_error("nanosecond coordinate overflow");
    return static_cast<std::int64_t>(result);
}

std::optional<std::pair<std::int64_t, std::int64_t>> elapsed_bounds(
    const std::optional<TimePoint>& first, const std::optional<TimePoint>& second) {
    if (!first || !second || first->clock != second->clock) return std::nullopt;
    const auto delta = second->ns - first->ns;
    const auto error = first->uncertainty_ns + second->uncertainty_ns;
    return std::pair{delta - error, delta + error};
}

std::string_view temporal_relation(
    const std::optional<TimePoint>& first, const std::optional<TimePoint>& second) {
    const auto interval = elapsed_bounds(first, second);
    if (!interval) return "unknown";
    if (interval->first > 0) return "before";
    if (interval->second < 0) return "after";
    if (interval->first == 0 && interval->second == 0) return "same_coordinate";
    return "uncertain_order";
}

TemporalEvent::TemporalEvent(
    std::string id, std::optional<TimePoint> occurred_value, TimePoint observed_value,
    std::string source_value, std::string kind_value, std::string outcome_value,
    std::vector<std::byte> payload_value)
    : event_id(std::move(id)), occurred(std::move(occurred_value)),
      observed(std::move(observed_value)), source(std::move(source_value)),
      kind(std::move(kind_value)), outcome(std::move(outcome_value)),
      payload(std::move(payload_value)) {
    if (!nonblank(event_id) || !nonblank(source))
        throw std::invalid_argument("identity and provenance required");
    if (kind != "change" && kind != "observed_unchanged" && kind != "observation_gap")
        throw std::invalid_argument("explicit observation kind required");
    static const std::vector<std::string_view> outcomes{
        "success", "failure", "negative", "uncertain", "conflict", "pending"};
    if (std::ranges::find(outcomes, outcome) == outcomes.end())
        throw std::invalid_argument("invalid outcome label");
}

std::vector<std::int64_t> TimeAxisBlock::values(
    const TimeAxisMode mode, const std::int64_t rate) const {
    const auto data = blob->read().data;
    const auto width = mode == TimeAxisMode::ticks_residual ? 12U : 8U;
    if (data.size() != count * width) throw CorruptLosslessBlock("time block layout changed");
    std::vector<std::int64_t> result;
    result.reserve(count);
    for (std::size_t at = 0; at != data.size(); at += width) {
        const auto q = get_u64(data, at);
        if (q > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            throw std::overflow_error("time delta overflow");
        if (mode == TimeAxisMode::nanoseconds) result.push_back(first + static_cast<std::int64_t>(q));
        else if (mode == TimeAxisMode::ticks)
            result.push_back(first + static_cast<std::int64_t>(q) * temporal_ns_per_second / rate);
        else result.push_back(first + join_tick(static_cast<std::int64_t>(q), get_u32(data, at + 8), rate));
    }
    return result;
}

PackedTimeAxis::PackedTimeAxis(std::string clock_value, const TimeAxisMode mode_value,
                               const std::int64_t rate_value,
                               std::vector<TimeAxisBlock> chunk_values)
    : clock(std::move(clock_value)), mode(mode_value), rate(rate_value),
      chunks(std::move(chunk_values)), ends([&] {
          std::vector<std::int64_t> values;
          for (const auto& chunk : chunks) values.push_back(chunk.last);
          return values;
      }()) {}

PackedTimeAxis PackedTimeAxis::build(
    const std::span<const std::int64_t> times, std::string clock,
    const TimeAxisMode mode, const std::size_t chunk_rows, const std::int64_t rate) {
    if (!nonblank(clock)) throw std::invalid_argument("clock required");
    if (rate < 1 || rate > temporal_ns_per_second)
        throw std::invalid_argument("rate must fit unsigned 64-bit delta coordinates");
    if (chunk_rows < 1 || chunk_rows > 1024)
        throw std::invalid_argument("bounded chunk size required");
    if (!std::ranges::is_sorted(times))
        throw std::invalid_argument("timestamps must be nondecreasing");
    std::vector<TimeAxisBlock> chunks;
    for (std::size_t start = 0; start < times.size(); start += chunk_rows) {
        const auto count = std::min(chunk_rows, times.size() - start);
        std::vector<std::byte> raw;
        for (std::size_t i = 0; i != count; ++i) {
            const auto delta = times[start + i] - times[start];
            if (mode == TimeAxisMode::nanoseconds) put_u64(raw, static_cast<std::uint64_t>(delta));
            else {
                const auto [q, residual] = split_tick(delta, rate);
                put_u64(raw, static_cast<std::uint64_t>(q));
                if (mode == TimeAxisMode::ticks_residual) put_u32(raw, static_cast<std::uint32_t>(residual));
            }
        }
        auto last = times[start + count - 1];
        if (mode == TimeAxisMode::ticks) {
            const auto q = split_tick(last - times[start], rate).first;
            last = times[start] + q * temporal_ns_per_second / rate;
        }
        chunks.push_back({times[start], last, count,
            LosslessBlob::build(raw, LosslessBlockCodec::zlib, 3, 16384)});
    }
    return PackedTimeAxis(std::move(clock), mode, rate, std::move(chunks));
}

std::vector<std::int64_t> PackedTimeAxis::read_range(
    const std::int64_t lower, const std::int64_t upper) const {
    if (lower > upper) throw std::invalid_argument("inverted range");
    std::vector<std::int64_t> result;
    auto index = static_cast<std::size_t>(std::lower_bound(ends.begin(), ends.end(), lower) - ends.begin());
    for (; index < chunks.size() && chunks[index].first <= upper; ++index)
        for (const auto value : chunks[index].values(mode, rate))
            if (value >= lower && value <= upper) result.push_back(value);
    return result;
}

PackedTimeAxis PackedTimeAxis::append(
    const std::span<const std::int64_t> times, const std::string_view domain) const {
    if (domain != clock) throw std::invalid_argument("clock domain changed; no implicit epoch bridge");
    if (times.empty()) return *this;
    if (mode == TimeAxisMode::ticks)
        throw std::invalid_argument("incremental exact snapshots require lossless mode");
    if (!chunks.empty() && times.front() < chunks.back().last)
        throw std::invalid_argument("late occurrence insertion is outside this prototype");
    auto added = build(times, clock, mode, 128, rate);
    auto all = chunks;
    all.insert(all.end(), added.chunks.begin(), added.chunks.end());
    return PackedTimeAxis(clock, mode, rate, std::move(all));
}

std::size_t PackedTimeAxis::payload_bytes() const noexcept {
    std::size_t value = 0; for (const auto& chunk : chunks) value += chunk.blob->stored_payload_bytes(); return value;
}
std::size_t PackedTimeAxis::raw_coordinate_bytes() const noexcept {
    std::size_t value = 0; for (const auto& chunk : chunks) value += chunk.blob->raw_size; return value;
}

}  // namespace swegca::world
