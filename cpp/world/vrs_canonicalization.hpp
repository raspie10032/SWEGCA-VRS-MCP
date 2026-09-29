#pragma once

#include "world/vrs_edge_address_index.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace swegca::world {

class SparseCanonicalLineage;

inline constexpr std::string_view vrs_canonicalization_source_sha256 =
    "979c9a470e99814b91571c243ee339f657d810665943c3b9b5032dca66ea6614";
inline constexpr std::string_view canonical_vrs_member_schema =
    "rozephine-canonical-vrs-edge-members-v1";

// Immutable bidirectional mapping between every logical input edge and the
// canonical endpoint/sign group that contains it.
class CanonicalVrsMemberLineage final {
public:
    CanonicalVrsMemberLineage(
        std::vector<std::uint32_t> member_edge_to_group,
        std::vector<std::uint64_t> group_member_offsets,
        std::vector<std::uint32_t> group_member_edge_ids);

    [[nodiscard]] const CanonicalVrsMemberLineage& require_validated_immutable() const noexcept;
    [[nodiscard]] std::size_t group_count() const noexcept;
    [[nodiscard]] std::size_t member_count() const noexcept;
    [[nodiscard]] std::uint64_t member_count_for(std::size_t group) const;

    [[nodiscard]] std::span<const std::uint32_t> member_edge_to_group() const noexcept;
    [[nodiscard]] std::span<const std::uint64_t> group_member_offsets() const noexcept;
    [[nodiscard]] std::span<const std::uint32_t> group_member_edge_ids() const noexcept;

    [[nodiscard]] CanonicalVrsMemberLineage append_members(
        std::span<const std::uint32_t> group_ids,
        std::uint64_t new_group_count) const;

private:
    struct TrustedAppend final {};
    CanonicalVrsMemberLineage(
        TrustedAppend,
        std::vector<std::uint32_t> member_edge_to_group,
        std::vector<std::uint64_t> group_member_offsets,
        std::vector<std::uint32_t> group_member_edge_ids);

    const std::vector<std::uint32_t> member_edge_to_group_;
    const std::vector<std::uint64_t> group_member_offsets_;
    const std::vector<std::uint32_t> group_member_edge_ids_;
};

struct CanonicalVrsGroupUpdate final {
    std::uint32_t group{};
    std::uint64_t member_count{};
    float base_strength{};
    float current_strength{};
};

struct CanonicalVrsGroupNewMembers final {
    std::uint32_t group{};
    std::vector<std::uint32_t> member_edge_ids;
};

// Numerical and membership proposal only. It owns no commit or publication
// authority and does not replace the parent generation.
struct CanonicalVrsAppendDelta final {
    std::vector<std::uint32_t> appended_member_group_ids;
    std::vector<EventSignalEdge> new_group_rows;
    std::vector<CanonicalVrsGroupUpdate> group_updates;
    std::vector<CanonicalVrsGroupNewMembers> group_new_members;
};

struct CanonicalVrsExtensionSummary final {
    std::uint64_t parent_canonical_group_count{};
    std::uint64_t parent_logical_member_count{};
    std::uint64_t appended_logical_member_count{};
    std::uint64_t appended_member_merged_into_existing_or_new_group_count{};
    std::uint64_t new_canonical_group_count{};
    std::uint64_t successor_canonical_group_count{};
    std::uint64_t successor_logical_member_count{};
    bool all_parent_members_preserved{};
    bool all_appended_rows_preserved_as_logical_members{};
    std::uint64_t pre_convergence_member_pruning{};
    bool parent_key_index_reused{};
    std::uint64_t parent_key_rows_sorted_this_call{};
};

struct CanonicalVrsExtension final {
    CanonicalVrsExtension(
        std::vector<EventSignalEdge> edges,
        std::vector<float> strengths,
        CanonicalVrsMemberLineage lineage,
        std::vector<std::uint32_t> appended_member_group_ids,
        CanonicalVrsExtensionSummary summary);

    const std::vector<EventSignalEdge> edges;
    const std::vector<float> strengths;
    const CanonicalVrsMemberLineage lineage;
    const std::vector<std::uint32_t> appended_member_group_ids;
    const CanonicalVrsExtensionSummary summary;
};

[[nodiscard]] CanonicalVrsAppendDelta prepare_canonical_vrs_append_delta(
    const PersistentEventVector<EventSignalEdge>& parent_edges,
    const PersistentEventVector<float>& parent_strengths,
    const CanonicalVrsMemberLineage& parent_lineage,
    std::span<const EventSignalEdge> appended_edges,
    std::span<const float> appended_strengths,
    const CanonicalEdgeAddressIndex& parent_address_index);

[[nodiscard]] CanonicalVrsAppendDelta prepare_canonical_vrs_append_delta(
    const PersistentEventVector<EventSignalEdge>& parent_edges,
    const PersistentEventVector<float>& parent_strengths,
    const SparseCanonicalLineage& parent_lineage,
    std::span<const EventSignalEdge> appended_edges,
    std::span<const float> appended_strengths,
    const CanonicalEdgeAddressIndex& parent_address_index);

// Legacy combined-prefix boundary retained by the pinned Python source. The
// sparse implementation above does the actual grouping work.
[[nodiscard]] CanonicalVrsExtension canonicalize_appended_vrs_edges(
    const PersistentEventVector<EventSignalEdge>& parent_edges,
    const PersistentEventVector<float>& parent_strengths,
    const CanonicalVrsMemberLineage& parent_lineage,
    std::span<const EventSignalEdge> combined_edges,
    std::span<const float> combined_strengths,
    std::shared_ptr<const CanonicalEdgeAddressIndex> parent_address_index = {});

}  // namespace swegca::world
