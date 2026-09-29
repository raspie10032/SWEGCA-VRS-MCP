#include "world/semantic_event_append.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace swegca::world;

namespace {

template<class Function>
bool rejects(Function&& function) {
    try { function(); }
    catch (const std::invalid_argument&) { return true; }
    return false;
}

SemanticSourceEpisode source() {
    return {
        "source:alpha",
        {"alpha"},
        {{"observe", JsonValue::Object{{"source_item_id", "alpha"}}, {},
          "fixture", "success", {"file:a"}}},
        {"file:a"},
        "revision:1",
        "verified-fixture",
    };
}

SemanticEncoding proposal(const SemanticSourceEpisode& original, std::string model) {
    const SemanticAnchor anchor{
        "item", 0, {std::string("source_item_id")}, "text", "original", {0, 5}, {}, {}};
    const SemanticMeaningUnit unit{
        "source item", "recorded identifier", "alpha", "affirmed", "reported",
        {"item"}, {}, "unspecified"};
    return {
        original.episode_id, original.revision, semantic_source_digest(original),
        original.source_addresses, {original.steps.front().outcome}, std::move(model),
        {anchor}, {unit}, {}, {}, {},
    };
}

struct Parent final {
    SemanticSourceEpisode source;
    std::shared_ptr<const EventSignalInputs> inputs;
    std::shared_ptr<const TermAddressIndex> terms;
    std::shared_ptr<const CanonicalVrsMemberLineage> lineage;
    std::shared_ptr<const CanonicalEdgeAddressIndex> edges;
};

Parent parent() {
    auto original = source();
    auto inputs = std::make_shared<const EventSignalInputs>(
        std::string(64, 'a'), std::vector<float>{-.25F, .25F},
        std::vector<float>{-.5F, .5F},
        std::vector<EventSignalEdge>{{0, 1, 1, 1.F}, {1, 0, -1, .5F}},
        std::vector<float>{1.F, .5F}, std::vector<std::uint8_t>{0, 0});
    auto terms = TermAddressIndex::build({original.episode_id, "unrelated"});
    auto lineage = std::make_shared<const CanonicalVrsMemberLineage>(
        std::vector<std::uint32_t>{0, 1}, std::vector<std::uint64_t>{0, 1, 2},
        std::vector<std::uint32_t>{0, 1});
    auto edges = CanonicalEdgeAddressIndex::build(inputs->edges);
    return {std::move(original), std::move(inputs), std::move(terms),
            std::move(lineage), std::move(edges)};
}

void test_first_append_wires_all_sparse_successors_without_settlement() {
    const auto fixture = parent();
    const auto encoding = proposal(fixture.source, "worker-one");
    const auto result = prepare_semantic_event_append(
        fixture.inputs, fixture.terms, fixture.lineage, fixture.edges,
        {fixture.source}, {encoding}, std::string(64, 'b'));

    assert(result.parent.get() == fixture.inputs.get());
    assert(result.inputs->delta_parent() == fixture.inputs.get());
    assert(result.semantic_delta.address_index.get() == fixture.terms.get());
    assert(result.address_index->size() == 5);
    assert(result.inputs->score.size() == 5 && result.inputs->direct.size() == 5 &&
           result.inputs->unresolved.size() == 5);
    for (std::size_t node = 2; node < 5; ++node)
        assert(result.inputs->score[node] == 0.F && result.inputs->direct[node] == 0.F &&
               result.inputs->unresolved[node] == 0);
    assert(result.inputs->edges.size() == 6 && result.inputs->strength.size() == 6);
    assert(result.lineage.base().get() == fixture.lineage.get());
    assert(result.lineage.group_count() == 6 && result.lineage.member_count() == 6);
    for (std::size_t member = 0; member < 6; ++member)
        assert(result.lineage.member_group(member) == member);
    for (std::size_t group = 2; group < 6; ++group) {
        assert(result.inputs->edges[group] == result.semantic_delta.edge_rows[group - 2]);
        assert(result.inputs->strength[group] == .75F);
        assert(result.edge_address_index->lookup(
            result.inputs->edges[group].source, result.inputs->edges[group].target,
            result.inputs->edges[group].sign) == group);
    }
    assert(fixture.inputs->score.size() == 2 && fixture.inputs->edges.size() == 2);
    assert(fixture.terms->size() == 2 && fixture.lineage->member_count() == 2);

    const auto receipt = result.receipt();
    assert(receipt.schema == semantic_event_append_schema);
    assert(receipt.parent_memory_snapshot_id == std::string(64, 'a'));
    assert(receipt.candidate_snapshot_id == std::string(64, 'b'));
    assert(receipt.added_terms == 3 && receipt.added_canonical_groups == 4);
    assert(receipt.added_logical_members == 4 && receipt.touched_canonical_groups == 4);
    assert(receipt.semantic_proposals.size() == 1);
    assert(receipt.semantic_proposals.front().canonical_edge_ids ==
           std::vector<std::uint32_t>({2, 3, 4, 5}));
    assert(receipt.stages_ns.size() == 5);
    assert(receipt.stages_ns[0].first == "source_bound_semantic_delta");
    assert(receipt.stages_ns[1].first == "canonical_changed_groups");
    assert(receipt.stages_ns[2].first == "shared_terms_and_membership");
    assert(receipt.stages_ns[3].first == "sparse_numeric_inputs");
    assert(receipt.stages_ns[4].first == "canonical_address_extension");
    assert(receipt.original_episode_reingestions() == 0);
    assert(receipt.new_observation_count() == 0);
    assert(receipt.independent_evidence_count() == 0);
    assert(receipt.parent_numeric_materializations() == 0);
    assert(receipt.parent_term_enumerations() == 0);
    assert(receipt.parent_lineage_materializations() == 0);
    assert(receipt.internal_llm_calls() == 0);
    assert(!receipt.main_pair_committed() && !receipt.authority_granted());
    assert(!receipt.signal_settled() && !receipt.whole_graph_convergence_claimed());
}

void test_successive_sparse_append_reuses_coordinates_and_merges_members() {
    const auto fixture = parent();
    const auto first = prepare_semantic_event_append(
        fixture.inputs, fixture.terms, fixture.lineage, fixture.edges,
        {fixture.source}, {proposal(fixture.source, "worker-one")},
        std::string(64, 'b'));
    const auto second = prepare_semantic_event_append(
        first.inputs, first.address_index, first.lineage, first.edge_address_index,
        {fixture.source}, {proposal(fixture.source, "worker-two")},
        std::string(64, 'c'));

    assert(second.semantic_delta.appended_terms.size() == 1);
    assert(second.address_index->size() == first.address_index->size() + 1);
    assert(second.address_index->terms()->cold_base() ==
           first.address_index->terms()->cold_base());
    assert(second.canonical_delta.appended_member_group_ids ==
           std::vector<std::uint32_t>({6, 3, 7, 5}));
    assert(second.inputs->edges.size() == first.inputs->edges.size() + 2);
    assert(second.lineage.group_count() == first.lineage.group_count() + 2);
    assert(second.lineage.member_count() == first.lineage.member_count() + 4);
    assert(second.lineage.base().get() == fixture.lineage.get());
    assert(second.lineage.members(3) == std::vector<std::uint32_t>({3, 7}));
    assert(second.lineage.members(5) == std::vector<std::uint32_t>({5, 9}));
    const auto first_semantic = first.semantic_delta.receipts().front();
    const auto second_semantic = second.semantic_delta.receipts().front();
    assert(first_semantic.anchor_nodes == second_semantic.anchor_nodes);
    assert(first_semantic.unit_nodes == second_semantic.unit_nodes);
    assert(first_semantic.node_id != second_semantic.node_id);
    assert(second.receipt().semantic_proposals.front().canonical_edge_ids ==
           std::vector<std::uint32_t>({6, 3, 7, 5}));
}

void test_two_specialists_in_one_wave_keep_alias_members_not_duplicate_groups() {
    const auto fixture = parent();
    const auto result = prepare_semantic_event_append(
        fixture.inputs, fixture.terms, fixture.lineage, fixture.edges,
        {fixture.source},
        {proposal(fixture.source, "worker-one"), proposal(fixture.source, "worker-two")},
        std::string(64, 'b'));
    assert(result.semantic_delta.edge_rows.size() == 8);
    assert(result.canonical_delta.appended_member_group_ids.size() == 8);
    assert(result.canonical_delta.new_group_rows.size() == 6);
    assert(result.inputs->edges.size() == 8);
    assert(result.lineage.member_count() == 10);
    assert(result.lineage.group_count() == 8);
    const auto receipt = result.receipt();
    assert(receipt.semantic_proposals.size() == 2);
    assert(receipt.semantic_proposals[0].canonical_edge_ids ==
           std::vector<std::uint32_t>({2, 3, 4, 5}));
    assert(receipt.semantic_proposals[1].canonical_edge_ids ==
           std::vector<std::uint32_t>({6, 3, 7, 5}));
    assert(result.lineage.members(3) == std::vector<std::uint32_t>({3, 7}));
    assert(result.lineage.members(5) == std::vector<std::uint32_t>({5, 9}));
}

void test_existing_groups_receive_python_mean_and_float16_strength() {
    const auto original = source();
    const auto encoding = proposal(original, "worker-mean");
    const auto anchor_address = semantic_anchor_address(
        original.episode_id, original.revision, encoding.anchors.front());
    const auto unit_address = semantic_unit_address(
        original.episode_id, original.revision, encoding.units.front());
    const auto terms = TermAddressIndex::build(
        {original.episode_id, anchor_address, unit_address});
    const auto inputs = std::make_shared<const EventSignalInputs>(
        std::string(64, 'a'), std::vector<float>(3), std::vector<float>(3),
        std::vector<EventSignalEdge>{{1, 0, 1, .5F}, {2, 1, 1, .25F}},
        std::vector<float>{.25F, .5F}, std::vector<std::uint8_t>(3));
    const auto lineage = std::make_shared<const CanonicalVrsMemberLineage>(
        std::vector<std::uint32_t>{0, 1}, std::vector<std::uint64_t>{0, 1, 2},
        std::vector<std::uint32_t>{0, 1});
    const auto edges = CanonicalEdgeAddressIndex::build(inputs->edges);
    const auto result = prepare_semantic_event_append(
        inputs, terms, lineage, edges, {original}, {encoding}, std::string(64, 'b'));
    assert(result.semantic_delta.appended_terms.size() == 1);
    assert(result.canonical_delta.appended_member_group_ids ==
           std::vector<std::uint32_t>({2, 0, 3, 1}));
    assert(result.inputs->edges[0].vrs_strength == .625F);
    assert(result.inputs->strength[0] == .5F);
    assert(result.inputs->edges[1].vrs_strength == .5F);
    assert(result.inputs->strength[1] == .625F);
    assert(result.lineage.members(0) == std::vector<std::uint32_t>({0, 3}));
    assert(result.lineage.members(1) == std::vector<std::uint32_t>({1, 5}));
    assert(inputs->edges[0].vrs_strength == .5F && inputs->strength[0] == .25F);
}

void test_empty_append_and_invalid_generation_bindings() {
    const auto fixture = parent();
    const auto empty = prepare_semantic_event_append(
        fixture.inputs, fixture.terms, fixture.lineage, fixture.edges,
        {fixture.source}, {}, std::string(64, 'b'));
    assert(empty.address_index->size() == fixture.terms->size());
    assert(empty.inputs->edges.size() == fixture.inputs->edges.size());
    assert(empty.lineage.group_count() == fixture.lineage->group_count());
    assert(empty.lineage.member_count() == fixture.lineage->member_count());
    assert(empty.receipt().added_terms == 0 &&
           empty.receipt().added_logical_members == 0);

    assert(rejects([&] {
        (void)prepare_semantic_event_append(
            fixture.inputs, fixture.terms, fixture.lineage, fixture.edges,
            {fixture.source}, {}, fixture.inputs->snapshot_id);
    }));
    const auto wrong_terms = TermAddressIndex::build({fixture.source.episode_id});
    assert(rejects([&] {
        (void)prepare_semantic_event_append(
            fixture.inputs, wrong_terms, fixture.lineage, fixture.edges,
            {fixture.source}, {}, std::string(64, 'b'));
    }));
    const auto foreign_inputs = std::make_shared<const EventSignalInputs>(
        std::string(64, 'd'), fixture.inputs->direct.materialize(),
        fixture.inputs->score.materialize(), fixture.inputs->edges.materialize(),
        fixture.inputs->strength.materialize(), fixture.inputs->unresolved.materialize());
    const auto foreign_edges = CanonicalEdgeAddressIndex::build(foreign_inputs->edges);
    assert(rejects([&] {
        (void)prepare_semantic_event_append(
            fixture.inputs, fixture.terms, fixture.lineage, foreign_edges,
            {fixture.source}, {}, std::string(64, 'b'));
    }));
    assert(rejects([&] {
        (void)prepare_semantic_event_append(
            fixture.inputs, fixture.terms,
            std::shared_ptr<const CanonicalVrsMemberLineage>{}, fixture.edges,
            {fixture.source}, {}, std::string(64, 'b'));
    }));
}

void test_large_parent_remains_shared_across_semantic_append() {
    constexpr std::size_t edge_count = 100000;
    auto original = source();
    std::vector<std::string> terms{original.episode_id, "unrelated"};
    terms.reserve(edge_count + 2);
    for (std::size_t index = 0; index < edge_count; ++index)
        terms.push_back("base:" + std::to_string(index));
    const auto term_index = TermAddressIndex::build(std::move(terms));
    const auto cold_terms = term_index->terms()->cold_base();

    std::vector<EventSignalEdge> edge_rows;
    std::vector<std::uint32_t> edge_to_group(edge_count);
    std::vector<std::uint64_t> offsets(edge_count + 1);
    std::vector<std::uint32_t> members(edge_count);
    edge_rows.reserve(edge_count);
    for (std::size_t index = 0; index < edge_count; ++index) {
        const auto node = static_cast<std::uint32_t>(index + 2);
        edge_rows.push_back({node, node, 1, .5F});
        edge_to_group[index] = static_cast<std::uint32_t>(index);
        offsets[index] = index;
        members[index] = static_cast<std::uint32_t>(index);
    }
    offsets[edge_count] = edge_count;
    const auto inputs = std::make_shared<const EventSignalInputs>(
        std::string(64, 'a'), std::vector<float>(edge_count + 2, .25F),
        std::vector<float>(edge_count + 2, -.25F), std::move(edge_rows),
        std::vector<float>(edge_count, .5F),
        std::vector<std::uint8_t>(edge_count + 2, 0));
    const auto lineage = std::make_shared<const CanonicalVrsMemberLineage>(
        std::move(edge_to_group), std::move(offsets), std::move(members));
    const auto edge_index = CanonicalEdgeAddressIndex::build(inputs->edges);

    const auto result = prepare_semantic_event_append(
        inputs, term_index, lineage, edge_index, {original},
        {proposal(original, "worker-large")}, std::string(64, 'b'));
    assert(result.semantic_delta.appended_terms.size() == 3);
    assert(result.canonical_delta.appended_member_group_ids.size() == 4);
    assert(result.inputs->delta_parent() == inputs.get());
    assert(result.inputs->score[90000] == inputs->score[90000]);
    assert(result.address_index->terms()->cold_base() == cold_terms);
    assert(result.lineage.base().get() == lineage.get());
    assert(result.inputs->score.size() == edge_count + 5);
    assert(result.inputs->edges.size() == edge_count + 4);
    assert(inputs->score.size() == edge_count + 2 && inputs->edges.size() == edge_count);
}

}  // namespace

int main() {
    test_first_append_wires_all_sparse_successors_without_settlement();
    test_successive_sparse_append_reuses_coordinates_and_merges_members();
    test_two_specialists_in_one_wave_keep_alias_members_not_duplicate_groups();
    test_existing_groups_receive_python_mean_and_float16_strength();
    test_empty_append_and_invalid_generation_bindings();
    test_large_parent_remains_shared_across_semantic_append();
    assert(semantic_event_append_source_sha256 ==
           "816a39039c3b84733cd7b8d4d0b6d6486ca77528b30e144db74bd52d88a7a619");
    std::cout << "semantic event append tests passed\n";
}
