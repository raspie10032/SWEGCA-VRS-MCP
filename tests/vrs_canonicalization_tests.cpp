#include "world/vrs_canonicalization.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
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

template<class T>
bool equal(const std::span<const T> left, const std::vector<T>& right) {
    return left.size() == right.size() &&
        std::equal(left.begin(), left.end(), right.begin());
}

struct Fixture final {
    PersistentEventVector<EventSignalEdge> edges{
        std::vector<EventSignalEdge>{{1, 2, 1, .5F}, {3, 4, -1, .25F}, {5, 6, 0, 1.F}}};
    PersistentEventVector<float> strengths{std::vector<float>{.75F, .5F, .25F}};
    CanonicalVrsMemberLineage lineage{{0, 1, 0, 2}, {0, 2, 3, 4}, {0, 2, 1, 3}};
    std::shared_ptr<const CanonicalEdgeAddressIndex> address_index{
        CanonicalEdgeAddressIndex::build(edges)};
};

void test_lineage_validation_and_stable_append() {
    Fixture fixture;
    const std::vector<std::uint32_t> additions{0, 3, 3, 1, 4};
    const auto result = fixture.lineage.append_members(additions, 2);
    assert(equal(result.member_edge_to_group(),
                 std::vector<std::uint32_t>{0, 1, 0, 2, 0, 3, 3, 1, 4}));
    assert(equal(result.group_member_offsets(),
                 std::vector<std::uint64_t>{0, 3, 5, 6, 8, 9}));
    assert(equal(result.group_member_edge_ids(),
                 std::vector<std::uint32_t>{0, 2, 4, 1, 7, 3, 5, 6, 8}));
    assert(result.member_count_for(0) == 3 && result.member_count_for(4) == 1);

    const std::span<const std::uint32_t> none;
    const auto noop = fixture.lineage.append_members(none, 0);
    assert(equal(noop.member_edge_to_group(), std::vector<std::uint32_t>{0, 1, 0, 2}));
    assert(rejects([&] { (void)fixture.lineage.append_members(none, 1); }));
    const std::vector<std::uint32_t> outside{5};
    assert(rejects([&] { (void)fixture.lineage.append_members(outside, 1); }));

    assert(rejects([] { (void)CanonicalVrsMemberLineage({}, {0}, {}); }));
    assert(rejects([] { (void)CanonicalVrsMemberLineage({0, 0}, {0, 2}, {0, 0}); }));
    assert(rejects([] { (void)CanonicalVrsMemberLineage({0, 1}, {0, 1, 2}, {1, 0}); }));
}

void test_sparse_delta_and_exact_group_means() {
    Fixture fixture;
    const std::vector<EventSignalEdge> appended{
        {1, 2, 1, .75F}, {9, 10, 1, .4F}, {9, 10, 1, .6F},
        {3, 4, -1, .5F}, {11, 12, -1, .3F}};
    const std::vector<float> strengths{.25F, .8F, 1.F, .25F, .7F};
    const auto delta = prepare_canonical_vrs_append_delta(
        fixture.edges, fixture.strengths, fixture.lineage,
        appended, strengths, *fixture.address_index);
    assert(delta.appended_member_group_ids ==
           std::vector<std::uint32_t>({0, 3, 3, 1, 4}));
    assert(delta.new_group_rows ==
           std::vector<EventSignalEdge>({{9, 10, 1, .4F}, {11, 12, -1, .3F}}));
    assert(delta.group_updates.size() == 4);
    assert(delta.group_updates[0].group == 0 && delta.group_updates[0].member_count == 3);
    assert(delta.group_updates[0].base_strength == static_cast<float>((.5 * 2 + .75) / 3));
    assert(delta.group_updates[0].current_strength == static_cast<float>((.75 * 2 + .25) / 3));
    assert(delta.group_updates[1].group == 3 && delta.group_updates[1].member_count == 2);
    assert(delta.group_updates[1].base_strength == static_cast<float>((.4 + .6) / 2));
    assert(delta.group_updates[1].current_strength == static_cast<float>((.8 + 1.) / 2));
    assert(delta.group_updates[2].group == 1 && delta.group_updates[2].member_count == 2);
    assert(delta.group_updates[2].base_strength == .375F &&
           delta.group_updates[2].current_strength == .375F);
    assert(delta.group_updates[3].group == 4 && delta.group_updates[3].member_count == 1);
    assert(delta.group_new_members.size() == 4);
    assert(delta.group_new_members[0].group == 0 &&
           delta.group_new_members[0].member_edge_ids == std::vector<std::uint32_t>{4});
    assert(delta.group_new_members[1].group == 1 &&
           delta.group_new_members[1].member_edge_ids == std::vector<std::uint32_t>{7});
    assert(delta.group_new_members[2].group == 3 &&
           delta.group_new_members[2].member_edge_ids == std::vector<std::uint32_t>({5, 6}));
    assert(delta.group_new_members[3].group == 4 &&
           delta.group_new_members[3].member_edge_ids == std::vector<std::uint32_t>{8});
}

void test_full_extension_and_summary() {
    Fixture fixture;
    const std::vector<EventSignalEdge> appended{
        {1, 2, 1, .75F}, {9, 10, 1, .4F}, {9, 10, 1, .6F},
        {3, 4, -1, .5F}, {11, 12, -1, .3F}};
    const std::vector<float> appended_strengths{.25F, .8F, 1.F, .25F, .7F};
    auto combined_edges = fixture.edges.materialize();
    combined_edges.insert(combined_edges.end(), appended.begin(), appended.end());
    auto combined_strengths = fixture.strengths.materialize();
    combined_strengths.insert(
        combined_strengths.end(), appended_strengths.begin(), appended_strengths.end());
    const auto result = canonicalize_appended_vrs_edges(
        fixture.edges, fixture.strengths, fixture.lineage,
        combined_edges, combined_strengths, fixture.address_index);
    assert(result.edges.size() == 5 && result.strengths.size() == 5);
    assert(result.edges[0].vrs_strength == static_cast<float>((.5 * 2 + .75) / 3));
    assert(result.edges[1].vrs_strength == .375F);
    assert(result.edges[2] == EventSignalEdge({5, 6, 0, 1.F}));
    assert(result.edges[3].source == 9 && result.edges[3].target == 10 &&
           result.edges[3].sign == 1 &&
           result.edges[3].vrs_strength == static_cast<float>((.4 + .6) / 2));
    assert(result.edges[4] == EventSignalEdge({11, 12, -1, .3F}));
    assert(result.strengths[0] == static_cast<float>((.75 * 2 + .25) / 3));
    assert(result.strengths[1] == .375F && result.strengths[2] == .25F);
    assert(result.strengths[3] == static_cast<float>((.8 + 1.) / 2));
    assert(result.strengths[4] == .7F);
    assert(equal(result.lineage.group_member_edge_ids(),
                 std::vector<std::uint32_t>{0, 2, 4, 1, 7, 3, 5, 6, 8}));
    assert(result.summary.parent_canonical_group_count == 3);
    assert(result.summary.parent_logical_member_count == 4);
    assert(result.summary.appended_logical_member_count == 5);
    assert(result.summary.appended_member_merged_into_existing_or_new_group_count == 3);
    assert(result.summary.new_canonical_group_count == 2);
    assert(result.summary.successor_canonical_group_count == 5);
    assert(result.summary.successor_logical_member_count == 9);
    assert(result.summary.all_parent_members_preserved);
    assert(result.summary.all_appended_rows_preserved_as_logical_members);
    assert(result.summary.pre_convergence_member_pruning == 0);
    assert(result.summary.parent_key_index_reused);
    assert(result.summary.parent_key_rows_sorted_this_call == 0);

    const auto uncached = canonicalize_appended_vrs_edges(
        fixture.edges, fixture.strengths, fixture.lineage,
        combined_edges, combined_strengths);
    assert(uncached.edges == result.edges && uncached.strengths == result.strengths);
    assert(!uncached.summary.parent_key_index_reused);
    assert(uncached.summary.parent_key_rows_sorted_this_call == 3);
}

void test_rejections_and_following_wave() {
    Fixture fixture;
    const std::vector<EventSignalEdge> one{{1, 2, 1, .75F}};
    const std::vector<float> strength{.25F};
    const std::vector<float> empty;
    assert(rejects([&] { (void)prepare_canonical_vrs_append_delta(
        fixture.edges, fixture.strengths, fixture.lineage,
        one, empty, *fixture.address_index); }));
    const std::vector<float> negative{-1.F};
    assert(rejects([&] { (void)prepare_canonical_vrs_append_delta(
        fixture.edges, fixture.strengths, fixture.lineage,
        one, negative, *fixture.address_index); }));
    const std::vector<EventSignalEdge> nan_edge{
        {7, 8, 1, std::numeric_limits<float>::quiet_NaN()}};
    assert(rejects([&] { (void)prepare_canonical_vrs_append_delta(
        fixture.edges, fixture.strengths, fixture.lineage,
        nan_edge, strength, *fixture.address_index); }));

    PersistentEventVector<EventSignalEdge> unrelated_edges{
        std::vector<EventSignalEdge>{{1, 2, 1, .5F}, {3, 4, -1, .25F}, {5, 6, 0, 1.F}}};
    const auto wrong_index = CanonicalEdgeAddressIndex::build(unrelated_edges);
    assert(rejects([&] { (void)prepare_canonical_vrs_append_delta(
        fixture.edges, fixture.strengths, fixture.lineage,
        one, strength, *wrong_index); }));

    auto bad_prefix = fixture.edges.materialize();
    bad_prefix[0].target = 99;
    assert(rejects([&] { (void)canonicalize_appended_vrs_edges(
        fixture.edges, fixture.strengths, fixture.lineage,
        bad_prefix, fixture.strengths.materialize(), fixture.address_index); }));

    auto combined_edges = fixture.edges.materialize();
    combined_edges.push_back({9, 10, 1, .4F});
    auto combined_strengths = fixture.strengths.materialize();
    combined_strengths.push_back(.8F);
    auto first = canonicalize_appended_vrs_edges(
        fixture.edges, fixture.strengths, fixture.lineage,
        combined_edges, combined_strengths, fixture.address_index);
    PersistentEventVector<EventSignalEdge> next_edges(first.edges);
    PersistentEventVector<float> next_strengths(first.strengths);
    auto next_index = CanonicalEdgeAddressIndex::build(next_edges);
    auto wave_edges = first.edges;
    wave_edges.push_back({9, 10, 1, .6F});
    auto wave_strengths = first.strengths;
    wave_strengths.push_back(1.F);
    const auto second = canonicalize_appended_vrs_edges(
        next_edges, next_strengths, first.lineage,
        wave_edges, wave_strengths, next_index);
    assert(second.edges.size() == first.edges.size());
    assert(second.lineage.member_count() == first.lineage.member_count() + 1);
    assert(second.appended_member_group_ids == std::vector<std::uint32_t>{3});
    assert(second.lineage.member_count_for(3) == 2);
}

}  // namespace

int main() {
    test_lineage_validation_and_stable_append();
    test_sparse_delta_and_exact_group_means();
    test_full_extension_and_summary();
    test_rejections_and_following_wave();
    assert(vrs_canonicalization_source_sha256 ==
           "979c9a470e99814b91571c243ee339f657d810665943c3b9b5032dca66ea6614");
    assert(canonical_vrs_member_schema == "rozephine-canonical-vrs-edge-members-v1");
    std::cout << "VRS canonicalization tests passed\n";
}
