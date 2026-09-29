#include "world/vrs_sparse_lineage.hpp"

#include <limits>
#include <map>
#include <stdexcept>
#include <utility>

namespace swegca::world {
namespace {

[[noreturn]] void reject(const char* message) { throw std::invalid_argument(message); }

}  // namespace

struct SparseCanonicalLineage::MemberGroupNode final {
    std::map<std::uint8_t, std::shared_ptr<const MemberGroupNode>> children;
    std::shared_ptr<const std::uint32_t> value;
};

struct SparseCanonicalLineage::GroupMembersNode final {
    std::map<std::uint8_t, std::shared_ptr<const GroupMembersNode>> children;
    std::shared_ptr<const std::vector<std::uint32_t>> value;
};

namespace {

std::shared_ptr<const SparseCanonicalLineage::MemberGroupNode> put_member_group(
    const std::shared_ptr<const SparseCanonicalLineage::MemberGroupNode>& source,
    const std::uint32_t index, const std::uint32_t value, const int shift = 24) {
    auto next = source ? std::make_shared<SparseCanonicalLineage::MemberGroupNode>(*source) :
                         std::make_shared<SparseCanonicalLineage::MemberGroupNode>();
    const auto key = static_cast<std::uint8_t>((index >> shift) & 255U);
    if (shift == 0) {
        auto leaf = std::make_shared<SparseCanonicalLineage::MemberGroupNode>();
        leaf->value = std::make_shared<const std::uint32_t>(value);
        next->children[key] = std::move(leaf);
    } else {
        const auto found = source ? source->children.find(key) :
                                    next->children.end();
        const auto child = !source || found == source->children.end() ?
            std::shared_ptr<const SparseCanonicalLineage::MemberGroupNode>{} : found->second;
        next->children[key] = put_member_group(child, index, value, shift - 8);
    }
    return next;
}

std::shared_ptr<const SparseCanonicalLineage::GroupMembersNode> put_group_members(
    const std::shared_ptr<const SparseCanonicalLineage::GroupMembersNode>& source,
    const std::uint32_t index, std::shared_ptr<const std::vector<std::uint32_t>> value,
    const int shift = 24) {
    auto next = source ? std::make_shared<SparseCanonicalLineage::GroupMembersNode>(*source) :
                         std::make_shared<SparseCanonicalLineage::GroupMembersNode>();
    const auto key = static_cast<std::uint8_t>((index >> shift) & 255U);
    if (shift == 0) {
        auto leaf = std::make_shared<SparseCanonicalLineage::GroupMembersNode>();
        leaf->value = std::move(value);
        next->children[key] = std::move(leaf);
    } else {
        const auto found = source ? source->children.find(key) :
                                    next->children.end();
        const auto child = !source || found == source->children.end() ?
            std::shared_ptr<const SparseCanonicalLineage::GroupMembersNode>{} : found->second;
        next->children[key] = put_group_members(child, index, std::move(value), shift - 8);
    }
    return next;
}

const std::uint32_t* lookup_member_group(
    std::shared_ptr<const SparseCanonicalLineage::MemberGroupNode> tree,
    const std::uint32_t index) noexcept {
    for (const int shift : {24, 16, 8, 0}) {
        if (!tree) return nullptr;
        const auto found = tree->children.find(
            static_cast<std::uint8_t>((index >> shift) & 255U));
        if (found == tree->children.end()) return nullptr;
        tree = found->second;
    }
    return tree && tree->value ? tree->value.get() : nullptr;
}

std::shared_ptr<const std::vector<std::uint32_t>> lookup_group_members(
    std::shared_ptr<const SparseCanonicalLineage::GroupMembersNode> tree,
    const std::uint32_t index) noexcept {
    for (const int shift : {24, 16, 8, 0}) {
        if (!tree) return {};
        const auto found = tree->children.find(
            static_cast<std::uint8_t>((index >> shift) & 255U));
        if (found == tree->children.end()) return {};
        tree = found->second;
    }
    return tree ? tree->value : std::shared_ptr<const std::vector<std::uint32_t>>{};
}

}  // namespace

SparseCanonicalLineage SparseCanonicalLineage::append_impl(
    const std::shared_ptr<const CanonicalVrsMemberLineage>& base,
    const std::shared_ptr<const SparseCanonicalLineage::MemberGroupNode>& parent_member_groups,
    const std::shared_ptr<const SparseCanonicalLineage::GroupMembersNode>& parent_members,
    const std::size_t parent_group_count, const std::size_t parent_member_count,
    const std::span<const std::uint32_t> group_ids,
    const std::uint64_t new_group_count) {
    if (!base) reject("verified canonical parent lineage required");
    (void)base->require_validated_immutable();
    if (new_group_count >
            std::numeric_limits<std::uint64_t>::max() - parent_group_count ||
        group_ids.size() >
            std::numeric_limits<std::uint64_t>::max() - parent_member_count)
        reject("canonical member address exceeds uint32");
    const auto groups = static_cast<std::uint64_t>(parent_group_count) + new_group_count;
    const auto members = static_cast<std::uint64_t>(parent_member_count) + group_ids.size();
    if (groups > std::numeric_limits<std::uint32_t>::max() ||
        members > (std::uint64_t{1} << 32U))
        reject("canonical member address exceeds uint32");

    std::map<std::uint32_t, std::vector<std::uint32_t>> additions;
    auto member_groups = parent_member_groups;
    for (std::size_t offset = 0; offset < group_ids.size(); ++offset) {
        const auto group = group_ids[offset];
        if (group >= groups) reject("canonical member group outside successor");
        const auto member = static_cast<std::uint32_t>(parent_member_count + offset);
        additions[group].push_back(member);
        member_groups = put_member_group(member_groups, member, group);
    }
    for (std::uint64_t group = parent_group_count; group < groups; ++group)
        if (!additions.contains(static_cast<std::uint32_t>(group)))
            reject("canonical extension created an empty group");

    auto group_members = parent_members;
    for (auto& [group, added] : additions) {
        const auto previous = lookup_group_members(group_members, group);
        auto combined = std::make_shared<std::vector<std::uint32_t>>();
        combined->reserve((previous ? previous->size() : 0) + added.size());
        if (previous) combined->insert(combined->end(), previous->begin(), previous->end());
        combined->insert(combined->end(), added.begin(), added.end());
        group_members = put_group_members(group_members, group, std::move(combined));
    }
    return SparseCanonicalLineage(
        base, std::move(member_groups), std::move(group_members),
        static_cast<std::size_t>(groups), static_cast<std::size_t>(members));
}

SparseCanonicalLineage::SparseCanonicalLineage(
    std::shared_ptr<const CanonicalVrsMemberLineage> base,
    std::shared_ptr<const MemberGroupNode> member_edge_to_group,
    std::shared_ptr<const GroupMembersNode> members,
    const std::size_t group_count, const std::size_t member_count)
    : base_(std::move(base)), member_edge_to_group_(std::move(member_edge_to_group)),
      members_(std::move(members)), group_count_(group_count), member_count_(member_count) {
    if (!base_ || group_count_ < base_->group_count() ||
        member_count_ < base_->member_count())
        reject("invalid sparse canonical lineage");
}

SparseCanonicalLineage SparseCanonicalLineage::append(
    std::shared_ptr<const CanonicalVrsMemberLineage> parent,
    const std::span<const std::uint32_t> group_ids,
    const std::uint64_t new_group_count) {
    if (!parent) reject("verified canonical parent lineage required");
    const auto groups = parent->group_count();
    const auto members = parent->member_count();
    return append_impl(parent, {}, {}, groups, members, group_ids, new_group_count);
}

SparseCanonicalLineage SparseCanonicalLineage::append(
    const SparseCanonicalLineage& parent,
    const std::span<const std::uint32_t> group_ids,
    const std::uint64_t new_group_count) {
    (void)parent.require_validated_immutable();
    return append_impl(
        parent.base_, parent.member_edge_to_group_, parent.members_,
        parent.group_count_, parent.member_count_, group_ids, new_group_count);
}

const SparseCanonicalLineage&
SparseCanonicalLineage::require_validated_immutable() const noexcept {
    (void)base_->require_validated_immutable();
    return *this;
}

const std::shared_ptr<const CanonicalVrsMemberLineage>&
SparseCanonicalLineage::base() const noexcept { return base_; }

std::size_t SparseCanonicalLineage::group_count() const noexcept { return group_count_; }
std::size_t SparseCanonicalLineage::member_count() const noexcept { return member_count_; }

std::uint32_t SparseCanonicalLineage::member_group(const std::size_t member) const {
    if (member >= member_count_)
        throw std::out_of_range("canonical member outside directory");
    if (member < base_->member_count()) return base_->member_edge_to_group()[member];
    const auto value = lookup_member_group(
        member_edge_to_group_, static_cast<std::uint32_t>(member));
    if (!value) throw std::logic_error("incomplete sparse canonical member path");
    return *value;
}

std::uint64_t SparseCanonicalLineage::member_count_for(const std::size_t group) const {
    if (group >= group_count_)
        throw std::out_of_range("canonical group outside directory");
    const auto old = group < base_->group_count() ? base_->member_count_for(group) : 0;
    const auto added = lookup_group_members(members_, static_cast<std::uint32_t>(group));
    return old + (added ? added->size() : 0);
}

std::vector<std::uint32_t> SparseCanonicalLineage::members(const std::size_t group) const {
    (void)member_count_for(group);
    std::vector<std::uint32_t> result;
    if (group < base_->group_count()) {
        const auto offsets = base_->group_member_offsets();
        const auto ids = base_->group_member_edge_ids();
        const auto start = static_cast<std::size_t>(offsets[group]);
        const auto stop = static_cast<std::size_t>(offsets[group + 1]);
        result.insert(result.end(), ids.begin() + start, ids.begin() + stop);
    }
    const auto added = lookup_group_members(members_, static_cast<std::uint32_t>(group));
    if (added) result.insert(result.end(), added->begin(), added->end());
    return result;
}

}  // namespace swegca::world
