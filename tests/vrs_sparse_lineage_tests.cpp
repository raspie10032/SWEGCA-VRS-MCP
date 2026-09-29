#include "world/vrs_sparse_lineage.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

using namespace swegca::world;

namespace {

template<class Function>
bool rejects(Function&& function) {
    try { function(); }
    catch (const std::invalid_argument&) { return true; }
    return false;
}

std::shared_ptr<const CanonicalVrsMemberLineage> base_lineage() {
    return std::make_shared<const CanonicalVrsMemberLineage>(
        std::vector<std::uint32_t>{0, 1, 0, 2},
        std::vector<std::uint64_t>{0, 2, 3, 4},
        std::vector<std::uint32_t>{0, 2, 1, 3});
}

void test_dense_parent_append_preserves_all_members() {
    const auto base = base_lineage();
    const std::vector<std::uint32_t> groups{0, 3, 3, 1, 4};
    const auto sparse = SparseCanonicalLineage::append(base, groups, 2);
    assert(sparse.base().get() == base.get());
    assert(sparse.group_count() == 5);
    assert(sparse.member_count() == 9);
    assert(sparse.require_validated_immutable().base().get() == base.get());
    assert(sparse.members(0) == std::vector<std::uint32_t>({0, 2, 4}));
    assert(sparse.members(1) == std::vector<std::uint32_t>({1, 7}));
    assert(sparse.members(2) == std::vector<std::uint32_t>{3});
    assert(sparse.members(3) == std::vector<std::uint32_t>({5, 6}));
    assert(sparse.members(4) == std::vector<std::uint32_t>{8});
    for (std::size_t member = 0; member < base->member_count(); ++member)
        assert(sparse.member_group(member) == base->member_edge_to_group()[member]);
    for (std::size_t offset = 0; offset < groups.size(); ++offset)
        assert(sparse.member_group(base->member_count() + offset) == groups[offset]);
    assert(base->member_count() == 4 && base->member_count_for(0) == 2);
}

void test_following_waves_share_cold_base_and_accumulate_touched_groups() {
    const auto base = base_lineage();
    const std::vector<std::uint32_t> first_groups{0, 3, 3, 1, 4};
    const auto first = SparseCanonicalLineage::append(base, first_groups, 2);
    const std::vector<std::uint32_t> second_groups{3, 5, 0, 5};
    const auto second = SparseCanonicalLineage::append(first, second_groups, 1);
    assert(first.base().get() == base.get());
    assert(second.base().get() == base.get());
    assert(second.group_count() == 6 && second.member_count() == 13);
    assert(second.members(0) == std::vector<std::uint32_t>({0, 2, 4, 11}));
    assert(second.members(1) == std::vector<std::uint32_t>({1, 7}));
    assert(second.members(2) == std::vector<std::uint32_t>{3});
    assert(second.members(3) == std::vector<std::uint32_t>({5, 6, 9}));
    assert(second.members(4) == std::vector<std::uint32_t>{8});
    assert(second.members(5) == std::vector<std::uint32_t>({10, 12}));
    for (std::size_t member = 0; member < first.member_count(); ++member)
        assert(second.member_group(member) == first.member_group(member));
    for (std::size_t offset = 0; offset < second_groups.size(); ++offset)
        assert(second.member_group(first.member_count() + offset) == second_groups[offset]);

    const std::vector<std::uint32_t> none;
    const auto noop = SparseCanonicalLineage::append(second, none, 0);
    assert(noop.base().get() == base.get());
    assert(noop.group_count() == second.group_count());
    assert(noop.member_count() == second.member_count());
    assert(noop.members(3) == second.members(3));
}

void test_invalid_groups_and_ranges_rejected_without_parent_change() {
    const auto base = base_lineage();
    const std::vector<std::uint32_t> none;
    assert(rejects([&] { (void)SparseCanonicalLineage::append(base, none, 1); }));
    assert(rejects([&] {
        (void)SparseCanonicalLineage::append(
            base, none, std::numeric_limits<std::uint64_t>::max());
    }));
    const std::vector<std::uint32_t> outside{4};
    assert(rejects([&] { (void)SparseCanonicalLineage::append(base, outside, 1); }));
    assert(rejects([&] {
        (void)SparseCanonicalLineage::append(
            std::shared_ptr<const CanonicalVrsMemberLineage>{}, none, 0);
    }));
    assert(base->member_count() == 4 && base->group_count() == 3);

    const std::vector<std::uint32_t> valid{3};
    const auto sparse = SparseCanonicalLineage::append(base, valid, 1);
    assert(rejects([&] { (void)SparseCanonicalLineage::append(sparse, none, 1); }));
    bool member_range = false;
    try { (void)sparse.member_group(sparse.member_count()); }
    catch (const std::out_of_range&) { member_range = true; }
    assert(member_range);
    bool group_range = false;
    try { (void)sparse.members(sparse.group_count()); }
    catch (const std::out_of_range&) { group_range = true; }
    assert(group_range);
}

void test_large_cold_lineage_is_shared_not_rewritten() {
    constexpr std::size_t count = 100000;
    std::vector<std::uint32_t> edge_to_group(count);
    std::vector<std::uint64_t> offsets(count + 1);
    std::vector<std::uint32_t> members(count);
    for (std::size_t index = 0; index < count; ++index) {
        edge_to_group[index] = static_cast<std::uint32_t>(index);
        offsets[index] = index;
        members[index] = static_cast<std::uint32_t>(index);
    }
    offsets[count] = count;
    const auto base = std::make_shared<const CanonicalVrsMemberLineage>(
        std::move(edge_to_group), std::move(offsets), std::move(members));
    const std::vector<std::uint32_t> additions{0, static_cast<std::uint32_t>(count)};
    const auto sparse = SparseCanonicalLineage::append(base, additions, 1);
    assert(sparse.base().get() == base.get());
    assert(sparse.member_count() == count + 2 && sparse.group_count() == count + 1);
    assert(sparse.members(0) == std::vector<std::uint32_t>({0, count}));
    assert(sparse.members(count) == std::vector<std::uint32_t>{count + 1});
    assert(sparse.member_group(count) == 0);
    assert(sparse.member_group(count + 1) == count);
}

void test_canonical_delta_feeds_sparse_lineage_without_dense_rebuild() {
    const auto base = base_lineage();
    PersistentEventVector<EventSignalEdge> edges{
        std::vector<EventSignalEdge>{{1, 2, 1, .5F}, {3, 4, -1, .25F}, {5, 6, 0, 1.F}}};
    PersistentEventVector<float> strengths{std::vector<float>{.75F, .5F, .25F}};
    const auto index = CanonicalEdgeAddressIndex::build(edges);
    const std::vector<EventSignalEdge> appended{
        {1, 2, 1, .75F}, {9, 10, 1, .4F}, {9, 10, 1, .6F},
        {3, 4, -1, .5F}, {11, 12, -1, .3F}};
    const std::vector<float> appended_strengths{.25F, .8F, 1.F, .25F, .7F};
    const auto delta = prepare_canonical_vrs_append_delta(
        edges, strengths, *base, appended, appended_strengths, *index);
    const auto sparse = SparseCanonicalLineage::append(
        base, delta.appended_member_group_ids, delta.new_group_rows.size());
    const auto dense = base->append_members(
        delta.appended_member_group_ids, delta.new_group_rows.size());
    assert(sparse.group_count() == dense.group_count());
    assert(sparse.member_count() == dense.member_count());
    for (std::size_t member = 0; member < dense.member_count(); ++member)
        assert(sparse.member_group(member) == dense.member_edge_to_group()[member]);
    for (std::size_t group = 0; group < dense.group_count(); ++group) {
        const auto offsets = dense.group_member_offsets();
        const auto ids = dense.group_member_edge_ids();
        const std::vector<std::uint32_t> expected(
            ids.begin() + static_cast<std::size_t>(offsets[group]),
            ids.begin() + static_cast<std::size_t>(offsets[group + 1]));
        assert(sparse.members(group) == expected);
    }
    assert(sparse.base().get() == base.get());
}

}  // namespace

int main() {
    test_dense_parent_append_preserves_all_members();
    test_following_waves_share_cold_base_and_accumulate_touched_groups();
    test_invalid_groups_and_ranges_rejected_without_parent_change();
    test_large_cold_lineage_is_shared_not_rewritten();
    test_canonical_delta_feeds_sparse_lineage_without_dense_rebuild();
    assert(vrs_sparse_lineage_source_sha256 ==
           "aa9f0b19d1e84d7774ca1a6d9fe6b23ac0c7e3b0342a3fbacd209d7eac433773");
    std::cout << "VRS sparse canonical lineage tests passed\n";
}
