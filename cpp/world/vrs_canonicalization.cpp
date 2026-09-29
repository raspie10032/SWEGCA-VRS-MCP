#include "world/vrs_canonicalization.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace swegca::world {
namespace {

[[noreturn]] void reject(const char* message) { throw std::invalid_argument(message); }

using Address = std::tuple<std::uint32_t, std::uint32_t, std::int8_t>;

[[nodiscard]] Address address(const EventSignalEdge& edge) noexcept {
    return {edge.source, edge.target, edge.sign};
}

[[nodiscard]] bool finite_nonnegative(const float value) noexcept {
    return std::isfinite(value) && value >= 0.0F;
}

[[nodiscard]] bool same_float(const float left, const float right) noexcept {
    // np.array_equal treats the two zero signs as equal and every NaN as
    // unequal. Native equality has exactly those semantics.
    return left == right;
}

struct RunningMean final {
    std::uint64_t count{};
    double base_sum{};
    double current_sum{};
    std::vector<std::uint32_t> new_members;
};

CanonicalVrsAppendDelta prepare_delta(
    const PersistentEventVector<EventSignalEdge>& parent_edges,
    const PersistentEventVector<float>& parent_strengths,
    const CanonicalVrsMemberLineage& parent_lineage,
    const std::span<const EventSignalEdge> appended_edges,
    const std::span<const float> appended_strengths,
    const CanonicalEdgeAddressIndex& parent_address_index) {
    const auto group_count = parent_edges.size();
    std::map<Address, std::uint32_t> new_keys;
    std::map<std::uint32_t, RunningMean> sums;
    std::vector<std::uint32_t> touched_order;
    CanonicalVrsAppendDelta result;
    result.appended_member_group_ids.reserve(appended_edges.size());
    result.new_group_rows.reserve(appended_edges.size());
    const auto first_member = parent_lineage.member_count();

    for (std::size_t ordinal = 0; ordinal < appended_edges.size(); ++ordinal) {
        const auto& row = appended_edges[ordinal];
        auto found = parent_address_index.lookup(row.source, row.target, row.sign);
        std::uint32_t group{};
        if (found) {
            group = static_cast<std::uint32_t>(*found);
        } else {
            const auto key = address(row);
            const auto inserted = new_keys.emplace(
                key, static_cast<std::uint32_t>(group_count + result.new_group_rows.size()));
            group = inserted.first->second;
            if (inserted.second) result.new_group_rows.push_back(row);
        }
        result.appended_member_group_ids.push_back(group);

        auto [position, inserted] = sums.try_emplace(group);
        auto& state = position->second;
        if (inserted) {
            touched_order.push_back(group);
            if (group < group_count) {
                state.count = parent_lineage.member_count_for(group);
                state.base_sum = static_cast<double>(parent_edges[group].vrs_strength) *
                    static_cast<double>(state.count);
                state.current_sum = static_cast<double>(parent_strengths[group]) *
                    static_cast<double>(state.count);
            }
        }
        ++state.count;
        // Keep the pinned np.float64 add order for repeated groups and round
        // only once when emitting the float32 group update.
        state.base_sum += static_cast<double>(row.vrs_strength);
        state.current_sum += static_cast<double>(appended_strengths[ordinal]);
        state.new_members.push_back(static_cast<std::uint32_t>(first_member + ordinal));
    }

    result.group_updates.reserve(touched_order.size());
    for (const auto group : touched_order) {
        const auto& state = sums.at(group);
        result.group_updates.push_back({
            group,
            state.count,
            static_cast<float>(state.base_sum / static_cast<double>(state.count)),
            static_cast<float>(state.current_sum / static_cast<double>(state.count)),
        });
    }
    result.group_new_members.reserve(sums.size());
    for (auto& [group, state] : sums)
        result.group_new_members.push_back({group, std::move(state.new_members)});
    return result;
}

}  // namespace

CanonicalVrsMemberLineage::CanonicalVrsMemberLineage(
    std::vector<std::uint32_t> member_edge_to_group,
    std::vector<std::uint64_t> group_member_offsets,
    std::vector<std::uint32_t> group_member_edge_ids)
    : member_edge_to_group_(std::move(member_edge_to_group)),
      group_member_offsets_(std::move(group_member_offsets)),
      group_member_edge_ids_(std::move(group_member_edge_ids)) {
    if (group_member_offsets_.empty()) reject("canonical VRS member lineage changed");
    const auto groups = group_member_offsets_.size() - 1;
    const auto members = member_edge_to_group_.size();
    if (group_member_offsets_.front() != 0 ||
        group_member_offsets_.back() != members ||
        group_member_edge_ids_.size() != members || members == 0)
        reject("canonical VRS member lineage changed");
    for (std::size_t group = 0; group < groups; ++group)
        if (group_member_offsets_[group + 1] < group_member_offsets_[group])
            reject("canonical VRS member lineage changed");

    std::vector<bool> seen(members, false);
    for (const auto member : group_member_edge_ids_) {
        if (member >= members || seen[member])
            reject("canonical VRS member lineage changed");
        seen[member] = true;
    }
    if (std::ranges::find(seen, false) != seen.end())
        reject("canonical VRS member lineage changed");
    for (std::size_t group = 0; group < groups; ++group)
        for (std::uint64_t position = group_member_offsets_[group];
             position < group_member_offsets_[group + 1]; ++position)
            if (member_edge_to_group_[group_member_edge_ids_[position]] != group)
                reject("canonical VRS reverse member lineage changed");
}

CanonicalVrsMemberLineage::CanonicalVrsMemberLineage(
    TrustedAppend,
    std::vector<std::uint32_t> member_edge_to_group,
    std::vector<std::uint64_t> group_member_offsets,
    std::vector<std::uint32_t> group_member_edge_ids)
    : member_edge_to_group_(std::move(member_edge_to_group)),
      group_member_offsets_(std::move(group_member_offsets)),
      group_member_edge_ids_(std::move(group_member_edge_ids)) {}

const CanonicalVrsMemberLineage&
CanonicalVrsMemberLineage::require_validated_immutable() const noexcept { return *this; }

std::size_t CanonicalVrsMemberLineage::group_count() const noexcept {
    return group_member_offsets_.size() - 1;
}

std::size_t CanonicalVrsMemberLineage::member_count() const noexcept {
    return member_edge_to_group_.size();
}

std::uint64_t CanonicalVrsMemberLineage::member_count_for(const std::size_t group) const {
    if (group >= group_count()) throw std::out_of_range("canonical group outside directory");
    return group_member_offsets_[group + 1] - group_member_offsets_[group];
}

std::span<const std::uint32_t>
CanonicalVrsMemberLineage::member_edge_to_group() const noexcept {
    return member_edge_to_group_;
}

std::span<const std::uint64_t>
CanonicalVrsMemberLineage::group_member_offsets() const noexcept {
    return group_member_offsets_;
}

std::span<const std::uint32_t>
CanonicalVrsMemberLineage::group_member_edge_ids() const noexcept {
    return group_member_edge_ids_;
}

CanonicalVrsMemberLineage CanonicalVrsMemberLineage::append_members(
    const std::span<const std::uint32_t> group_ids,
    const std::uint64_t new_group_count) const {
    (void)require_validated_immutable();
    if (new_group_count >
            std::numeric_limits<std::uint64_t>::max() - group_count() ||
        group_ids.size() >
            std::numeric_limits<std::uint64_t>::max() - member_count())
        reject("canonical member append exceeds uint32 addressing");
    const auto groups = static_cast<std::uint64_t>(group_count()) + new_group_count;
    const auto members = static_cast<std::uint64_t>(member_count()) + group_ids.size();
    if (groups > std::numeric_limits<std::uint32_t>::max() ||
        members > (std::uint64_t{1} << 32U))
        reject("canonical member append exceeds uint32 addressing");

    std::vector<std::uint64_t> counts(groups, 0);
    for (std::size_t group = 0; group < group_count(); ++group)
        counts[group] = member_count_for(group);
    std::map<std::uint32_t, std::vector<std::uint32_t>> additions;
    for (std::size_t ordinal = 0; ordinal < group_ids.size(); ++ordinal) {
        const auto group = group_ids[ordinal];
        if (group >= groups) reject("canonical member group is outside successor");
        ++counts[group];
        additions[group].push_back(static_cast<std::uint32_t>(member_count() + ordinal));
    }
    if (std::ranges::find(counts, std::uint64_t{0}) != counts.end())
        reject("canonical VRS extension created an empty group");
    if (group_ids.empty()) return *this;

    auto edge_to_group = member_edge_to_group_;
    edge_to_group.insert(edge_to_group.end(), group_ids.begin(), group_ids.end());
    std::vector<std::uint32_t> member_ids;
    member_ids.reserve(static_cast<std::size_t>(members));
    std::size_t old_position = 0;
    for (const auto& [group, added_members] : additions) {
        const auto stop = group < group_count() ?
            static_cast<std::size_t>(group_member_offsets_[group + 1]) : member_count();
        member_ids.insert(member_ids.end(),
                          group_member_edge_ids_.begin() + old_position,
                          group_member_edge_ids_.begin() + stop);
        member_ids.insert(member_ids.end(), added_members.begin(), added_members.end());
        old_position = stop;
    }
    member_ids.insert(member_ids.end(),
                      group_member_edge_ids_.begin() + old_position,
                      group_member_edge_ids_.end());
    std::vector<std::uint64_t> offsets(groups + 1, 0);
    for (std::size_t group = 0; group < counts.size(); ++group)
        offsets[group + 1] = offsets[group] + counts[group];
    return CanonicalVrsMemberLineage(
        TrustedAppend{}, std::move(edge_to_group), std::move(offsets), std::move(member_ids));
}

CanonicalVrsExtension::CanonicalVrsExtension(
    std::vector<EventSignalEdge> edges_value,
    std::vector<float> strengths_value,
    CanonicalVrsMemberLineage lineage_value,
    std::vector<std::uint32_t> appended_member_group_ids_value,
    CanonicalVrsExtensionSummary summary_value)
    : edges(std::move(edges_value)), strengths(std::move(strengths_value)),
      lineage(std::move(lineage_value)),
      appended_member_group_ids(std::move(appended_member_group_ids_value)),
      summary(summary_value) {
    if (strengths.size() != edges.size() || lineage.group_count() != edges.size())
        reject("canonical VRS extension changed");
    for (const auto value : strengths)
        if (!finite_nonnegative(value)) reject("canonical VRS extension changed");
    for (const auto group : appended_member_group_ids)
        if (group >= edges.size()) reject("canonical VRS extension changed");
}

CanonicalVrsAppendDelta prepare_canonical_vrs_append_delta(
    const PersistentEventVector<EventSignalEdge>& parent_edges,
    const PersistentEventVector<float>& parent_strengths,
    const CanonicalVrsMemberLineage& parent_lineage,
    const std::span<const EventSignalEdge> appended_edges,
    const std::span<const float> appended_strengths,
    const CanonicalEdgeAddressIndex& parent_address_index) {
    parent_address_index.require_source(parent_edges);
    (void)parent_lineage.require_validated_immutable();
    if (parent_strengths.size() != parent_edges.size() ||
        parent_lineage.group_count() != parent_edges.size() ||
        appended_strengths.size() != appended_edges.size())
        reject("canonical append delta layout changed");
    for (std::size_t index = 0; index < appended_edges.size(); ++index)
        if (!finite_nonnegative(appended_edges[index].vrs_strength) ||
            !finite_nonnegative(appended_strengths[index]))
            reject("canonical append strengths must be finite nonnegative");
    if (parent_lineage.member_count() + appended_edges.size() >
        (std::uint64_t{1} << 32U))
        reject("canonical member append exceeds uint32 addressing");
    return prepare_delta(parent_edges, parent_strengths, parent_lineage,
                         appended_edges, appended_strengths, parent_address_index);
}

CanonicalVrsExtension canonicalize_appended_vrs_edges(
    const PersistentEventVector<EventSignalEdge>& parent_edges,
    const PersistentEventVector<float>& parent_strengths,
    const CanonicalVrsMemberLineage& parent_lineage,
    const std::span<const EventSignalEdge> combined_edges,
    const std::span<const float> combined_strengths,
    std::shared_ptr<const CanonicalEdgeAddressIndex> parent_address_index) {
    const auto group_count = parent_edges.size();
    if (parent_lineage.group_count() != group_count ||
        parent_strengths.size() != group_count ||
        combined_edges.size() < group_count ||
        combined_strengths.size() != combined_edges.size())
        reject("canonical VRS append prefix changed");
    for (std::size_t index = 0; index < group_count; ++index) {
        if (!(combined_edges[index] == parent_edges[index]) ||
            !same_float(combined_strengths[index], parent_strengths[index]))
            reject("canonical VRS append prefix changed");
    }
    const bool reused = static_cast<bool>(parent_address_index);
    if (!parent_address_index)
        parent_address_index = CanonicalEdgeAddressIndex::build(parent_edges);
    else
        parent_address_index->require_source(parent_edges);

    const auto appended_edges = combined_edges.subspan(group_count);
    const auto appended_strengths = combined_strengths.subspan(group_count);
    auto delta = prepare_canonical_vrs_append_delta(
        parent_edges, parent_strengths, parent_lineage,
        appended_edges, appended_strengths, *parent_address_index);

    auto output_edges = parent_edges.materialize();
    output_edges.insert(output_edges.end(),
                        delta.new_group_rows.begin(), delta.new_group_rows.end());
    auto output_strengths = parent_strengths.materialize();
    output_strengths.resize(output_edges.size());
    for (const auto& update : delta.group_updates) {
        output_edges[update.group].vrs_strength = update.base_strength;
        output_strengths[update.group] = update.current_strength;
    }
    const auto new_group_count = delta.new_group_rows.size();
    auto lineage = parent_lineage.append_members(
        delta.appended_member_group_ids, new_group_count);
    const auto appended_count = appended_edges.size();
    CanonicalVrsExtensionSummary summary{
        group_count,
        parent_lineage.member_count(),
        appended_count,
        appended_count - new_group_count,
        new_group_count,
        output_edges.size(),
        lineage.member_count(),
        true,
        true,
        0,
        reused,
        reused ? 0U : static_cast<std::uint64_t>(group_count),
    };
    return CanonicalVrsExtension(
        std::move(output_edges), std::move(output_strengths), std::move(lineage),
        std::move(delta.appended_member_group_ids), summary);
}

}  // namespace swegca::world
