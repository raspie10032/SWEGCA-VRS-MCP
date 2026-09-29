#include "world/packed_memberships.hpp"

#include <stdexcept>

namespace swegca::world {

PackedMemberships::PackedMemberships(std::vector<std::uint64_t> groups,
                                     std::vector<double> weights)
    : groups_(std::move(groups)), weights_(std::move(weights)) {
    if (groups_.size() != weights_.size())
        throw std::invalid_argument("equal immutable 64-bit columns required");
}

std::size_t PackedMemberships::size() const noexcept { return groups_.size(); }

MembershipValue PackedMemberships::operator[](const std::size_t index) const {
    if (index >= size()) throw std::out_of_range("membership index outside source");
    return {groups_[index], weights_[index]};
}

std::vector<MembershipValue> PackedMemberships::values() const {
    std::vector<MembershipValue> result;
    result.reserve(size());
    for (std::size_t index = 0; index < size(); ++index)
        result.push_back((*this)[index]);
    return result;
}

PackedMembershipResult pack_validated_memberships(
    const std::vector<MembershipValue>& values) {
    if (values.empty() ||
        !std::holds_alternative<std::uint64_t>(values.back().group))
        return values;
    std::vector<std::uint64_t> groups;
    std::vector<double> weights;
    groups.reserve(values.size());
    weights.reserve(values.size());
    for (const auto& value : values) {
        const auto* group = std::get_if<std::uint64_t>(&value.group);
        if (!group) return values;
        groups.push_back(*group);
        weights.push_back(value.weight);
    }
    return PackedMemberships(std::move(groups), std::move(weights));
}

}  // namespace swegca::world
