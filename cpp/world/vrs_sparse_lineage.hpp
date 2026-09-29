#pragma once

#include "world/vrs_canonicalization.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view vrs_sparse_lineage_source_sha256 =
    "aa9f0b19d1e84d7774ca1a6d9fe6b23ac0c7e3b0342a3fbacd209d7eac433773";

// Persistent canonical membership over one shared, already validated cold
// lineage. Only appended member addresses and touched groups receive new radix
// paths. This is an explicit sparse interface, not a materialized CSR view.
class SparseCanonicalLineage final {
public:
    struct MemberGroupNode;
    struct GroupMembersNode;

    [[nodiscard]] static SparseCanonicalLineage append(
        std::shared_ptr<const CanonicalVrsMemberLineage> parent,
        std::span<const std::uint32_t> group_ids,
        std::uint64_t new_group_count);

    [[nodiscard]] static SparseCanonicalLineage append(
        const SparseCanonicalLineage& parent,
        std::span<const std::uint32_t> group_ids,
        std::uint64_t new_group_count);

    [[nodiscard]] const SparseCanonicalLineage& require_validated_immutable() const noexcept;
    [[nodiscard]] const std::shared_ptr<const CanonicalVrsMemberLineage>& base() const noexcept;
    [[nodiscard]] std::size_t group_count() const noexcept;
    [[nodiscard]] std::size_t member_count() const noexcept;
    [[nodiscard]] std::uint32_t member_group(std::size_t member) const;
    [[nodiscard]] std::uint64_t member_count_for(std::size_t group) const;
    [[nodiscard]] std::vector<std::uint32_t> members(std::size_t group) const;

private:
    [[nodiscard]] static SparseCanonicalLineage append_impl(
        const std::shared_ptr<const CanonicalVrsMemberLineage>& base,
        const std::shared_ptr<const MemberGroupNode>& parent_member_groups,
        const std::shared_ptr<const GroupMembersNode>& parent_members,
        std::size_t parent_group_count,
        std::size_t parent_member_count,
        std::span<const std::uint32_t> group_ids,
        std::uint64_t new_group_count);

    SparseCanonicalLineage(
        std::shared_ptr<const CanonicalVrsMemberLineage> base,
        std::shared_ptr<const MemberGroupNode> member_edge_to_group,
        std::shared_ptr<const GroupMembersNode> members,
        std::size_t group_count,
        std::size_t member_count);

    const std::shared_ptr<const CanonicalVrsMemberLineage> base_;
    const std::shared_ptr<const MemberGroupNode> member_edge_to_group_;
    const std::shared_ptr<const GroupMembersNode> members_;
    const std::size_t group_count_;
    const std::size_t member_count_;
};

}  // namespace swegca::world
