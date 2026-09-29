#include "world/vrs_edge_address_index.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace swegca::world {
namespace {

struct Entry final {
    std::uint32_t source{};
    std::uint32_t target{};
    std::int8_t sign{};
    std::uint32_t id{};
};

[[nodiscard]] auto key(const Entry& row) noexcept {
    return std::tuple(row.source, row.target, row.sign);
}

[[noreturn]] void reject(const char* message) { throw std::invalid_argument(message); }

}  // namespace

struct CanonicalEdgeAddressIndex::Segment final {
    std::vector<std::uint32_t> sources;
    std::vector<std::uint32_t> targets;
    std::vector<std::int8_t> signs;
    std::vector<std::uint32_t> ids;

    [[nodiscard]] std::size_t size() const noexcept { return ids.size(); }
    [[nodiscard]] auto key_at(const std::size_t index) const noexcept {
        return std::tuple(sources[index], targets[index], signs[index]);
    }
};

namespace {

[[nodiscard]] std::shared_ptr<const CanonicalEdgeAddressIndex::Segment> make_segment(
    const PersistentEventVector<EventSignalEdge>& edges,
    const std::size_t begin, const std::size_t end) {
    std::vector<Entry> entries;
    entries.reserve(end - begin);
    for (std::size_t index = begin; index < end; ++index) {
        const auto edge = edges[index];
        entries.push_back({edge.source, edge.target, edge.sign,
                           static_cast<std::uint32_t>(index)});
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& left, const Entry& right) {
        return key(left) < key(right);
    });
    for (std::size_t index = 1; index < entries.size(); ++index)
        if (key(entries[index - 1]) == key(entries[index]))
            reject("canonical parent contains duplicate endpoint-sign groups");

    auto result = std::make_shared<CanonicalEdgeAddressIndex::Segment>();
    result->sources.reserve(entries.size());
    result->targets.reserve(entries.size());
    result->signs.reserve(entries.size());
    result->ids.reserve(entries.size());
    for (const auto& row : entries) {
        result->sources.push_back(row.source);
        result->targets.push_back(row.target);
        result->signs.push_back(row.sign);
        result->ids.push_back(row.id);
    }
    return result;
}

[[nodiscard]] std::shared_ptr<const CanonicalEdgeAddressIndex::Segment> merge_segments(
    const CanonicalEdgeAddressIndex::Segment& left,
    const CanonicalEdgeAddressIndex::Segment& right) {
    auto result = std::make_shared<CanonicalEdgeAddressIndex::Segment>();
    const auto count = left.size() + right.size();
    result->sources.reserve(count);
    result->targets.reserve(count);
    result->signs.reserve(count);
    result->ids.reserve(count);
    std::size_t a = 0;
    std::size_t b = 0;
    while (a < left.size() || b < right.size()) {
        const bool take_left = b == right.size() ||
            (a < left.size() && left.key_at(a) < right.key_at(b));
        const auto& source = take_left ? left : right;
        const auto index = take_left ? a++ : b++;
        result->sources.push_back(source.sources[index]);
        result->targets.push_back(source.targets[index]);
        result->signs.push_back(source.signs[index]);
        result->ids.push_back(source.ids[index]);
    }
    return result;
}

}  // namespace

CanonicalEdgeAddressIndex::CanonicalEdgeAddressIndex(
    PersistentEventVector<EventSignalEdge> source,
    std::vector<std::shared_ptr<const Segment>> segments)
    : source_(std::move(source)), segments_(std::move(segments)) {}

std::shared_ptr<const CanonicalEdgeAddressIndex> CanonicalEdgeAddressIndex::build(
    const PersistentEventVector<EventSignalEdge>& source) {
    if (source.size() > std::numeric_limits<std::uint32_t>::max())
        reject("edge IDs exceed uint32 address format");
    std::vector<std::shared_ptr<const Segment>> segments;
    if (source.size()) segments.push_back(make_segment(source, 0, source.size()));
    return std::shared_ptr<const CanonicalEdgeAddressIndex>(
        new CanonicalEdgeAddressIndex(source, std::move(segments)));
}

void CanonicalEdgeAddressIndex::require_source(
    const PersistentEventVector<EventSignalEdge>& source) const {
    if (!source_.same_representation(source))
        reject("address index belongs to a different immutable generation");
}

std::optional<std::size_t> CanonicalEdgeAddressIndex::lookup(
    const std::uint64_t source, const std::uint64_t target, const int sign) const {
    if (source > std::numeric_limits<std::uint32_t>::max() ||
        target > std::numeric_limits<std::uint32_t>::max() ||
        sign < std::numeric_limits<std::int8_t>::min() ||
        sign > std::numeric_limits<std::int8_t>::max())
        reject("address key out of range");
    const auto probe = std::tuple(
        static_cast<std::uint32_t>(source), static_cast<std::uint32_t>(target),
        static_cast<std::int8_t>(sign));
    for (const auto& segment : segments_) {
        std::size_t begin = 0;
        std::size_t end = segment->size();
        while (begin < end) {
            const auto middle = begin + (end - begin) / 2;
            if (segment->key_at(middle) < probe) begin = middle + 1;
            else end = middle;
        }
        if (begin < segment->size() && segment->key_at(begin) == probe)
            return segment->ids[begin];
    }
    return std::nullopt;
}

std::shared_ptr<const CanonicalEdgeAddressIndex> CanonicalEdgeAddressIndex::advance(
    const PersistentEventVector<EventSignalEdge>& successor) const {
    const auto count = source_.size();
    if (successor.size() < count ||
        successor.size() > std::numeric_limits<std::uint32_t>::max())
        reject("successor changed canonical address prefix");
    for (std::size_t index = 0; index < count; ++index) {
        const auto old = source_[index];
        const auto next = successor[index];
        if (old.source != next.source || old.target != next.target || old.sign != next.sign)
            reject("successor changed canonical address prefix");
    }

    auto segments = segments_;
    if (successor.size() != count) {
        auto added = make_segment(successor, count, successor.size());
        for (std::size_t index = 0; index < added->size(); ++index) {
            const auto address = lookup(
                added->sources[index], added->targets[index], added->signs[index]);
            if (address) reject("successor appended an existing canonical address");
        }
        segments.push_back(std::move(added));
        while (segments.size() > 2 &&
               2 * segments.back()->size() >= segments[segments.size() - 2]->size()) {
            auto combined = merge_segments(
                *segments[segments.size() - 2], *segments.back());
            segments.pop_back();
            segments.back() = std::move(combined);
        }
    }
    return std::shared_ptr<const CanonicalEdgeAddressIndex>(
        new CanonicalEdgeAddressIndex(successor, std::move(segments)));
}

std::shared_ptr<const CanonicalEdgeAddressIndex>
CanonicalEdgeAddressIndex::advance_event_delta(
    const EventSignalInputs& parent, const EventSignalInputs& successor) const {
    if (successor.delta_parent() != &parent)
        reject("producer-bound topology delta required");
    require_source(parent.edges);
    const auto count = parent.edges.size();
    if (successor.edges.size() > std::numeric_limits<std::uint32_t>::max())
        reject("edge IDs exceed uint32 address format");

    auto segments = segments_;
    if (successor.edges.size() != count) {
        auto added = make_segment(successor.edges, count, successor.edges.size());
        for (std::size_t index = 0; index < added->size(); ++index) {
            const auto address = lookup(
                added->sources[index], added->targets[index], added->signs[index]);
            if (address) reject("successor appended an existing canonical address");
        }
        segments.push_back(std::move(added));
        while (segments.size() > 2 &&
               2 * segments.back()->size() >= segments[segments.size() - 2]->size()) {
            auto combined = merge_segments(
                *segments[segments.size() - 2], *segments.back());
            segments.pop_back();
            segments.back() = std::move(combined);
        }
    }
    return std::shared_ptr<const CanonicalEdgeAddressIndex>(
        new CanonicalEdgeAddressIndex(successor.edges, std::move(segments)));
}

std::size_t CanonicalEdgeAddressIndex::segment_count() const noexcept {
    return segments_.size();
}

std::size_t CanonicalEdgeAddressIndex::index_bytes() const noexcept {
    std::size_t result = 0;
    for (const auto& segment : segments_)
        result += segment->size() *
            (sizeof(std::uint32_t) * 3 + sizeof(std::int8_t));
    return result;
}

}  // namespace swegca::world
