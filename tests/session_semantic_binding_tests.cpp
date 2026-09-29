#include "world/session_semantic_binding.hpp"

#include <algorithm>
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

SemanticSourceEpisode episode(std::string identifier) {
    return {std::move(identifier), {}, {}, {}, "revision", "fixture"};
}

SemanticAnchor anchor(std::string identifier, const std::int64_t index) {
    return {std::move(identifier), 0, {index}, "text", "original", {0, 3}, {}, {}};
}

std::shared_ptr<const BoundSessionSemantics> binding(
    std::string derivative_id = "session-semantic:one",
    std::string memory_snapshot_id = "memory-1") {
    const auto first = anchor("event-0-block-0", 0);
    const auto second = anchor("event-1-block-0", 1);
    const SemanticMeaningUnit first_unit{
        "문", "확인요청", true, "affirmed", "reported",
        {first.identifier}, {}, "literal"};
    const SemanticMeaningUnit second_unit{
        "문", "현재잠금", nullptr, "unknown", "reported",
        {second.identifier}, {}, "literal"};
    std::vector<SessionSemanticUnitBinding> units{
        {first_unit, {first}, {}, {}, {}, {}},
        {second_unit, {second}, {}, {}, {}, {second.identifier}},
    };
    return std::make_shared<const BoundSessionSemantics>(BoundSessionSemantics{
        std::move(memory_snapshot_id), {"sess", "rev", "turn"},
        JsonValue::Object{{"model", "fixture"}},
        {episode("parent:0"), episode("parent:1"), episode("parent:2")},
        episode(std::move(derivative_id)), {first, second}, std::move(units),
        {second.identifier},
    });
}

std::shared_ptr<const TermAddressIndex> parent_index() {
    return TermAddressIndex::build(
        {"parent:0", "parent:1", "parent:2", "unrelated"});
}

void test_python_address_parity() {
    const JsonValue scope = JsonValue::Array{"sess", "rev", "turn"};
    const auto first = anchor("event-0-block-0", 0);
    const SemanticMeaningUnit unit{
        "문", "확인요청", true, "affirmed", "reported",
        {first.identifier}, {}, "literal"};
    assert(semantic_scoped_address("session-document", scope, scope) ==
        "session-document:2339d127150dd9bb15a2b7b9292790fff7e29d7d6ec76f68426f758fec828c96");
    assert(semantic_anchor_address(scope, first) ==
        "semantic-anchor:e8e52d6fff3261cc302983cd90ed21f596d5d9921fd4730ea3c8215f713b3065");
    assert(semantic_unit_address(scope, unit) ==
        "semantic-unit:a1573c157bd3865c7d1da05e0741edda04272373ae72140f16d728108a30ebb9");
}

void test_sparse_session_graph_preserves_all_parents_and_roles() {
    const auto source = binding();
    const auto index = parent_index();
    const auto delta = prepare_session_semantic_delta(
        source, index, 7, "memory-1", "vrs-1");

    assert(delta.address_index.get() == index.get());
    assert(index->size() == 4);
    assert(delta.appended_terms.size() == 6);
    assert(delta.edge_rows.size() == 11);
    assert(delta.edge_rows.size() == delta.edge_roles.size());
    assert(std::ranges::count_if(delta.edge_roles, [](const auto& role) {
        return role.kind == "document_reconstruction_parent";
    }) == 3);
    assert(std::ranges::any_of(delta.edge_roles, [](const auto& role) {
        return role.kind == "unresolved_anchor";
    }));
    assert(std::ranges::none_of(delta.edge_roles, [](const auto& role) {
        return role.kind == "anchor_source_location";
    }));
    for (const auto& edge : delta.edge_rows)
        assert(edge.sign == 1 && edge.vrs_strength == .75F);
    assert(delta.unit_graph_addresses.size() == 2);
    assert(delta.unit_graph_addresses[0].size() == 4);
    assert(delta.unit_graph_addresses[1].size() == 4);
    assert(!delta.associations_are_logical_implications() && !delta.grants_authority());

    const auto receipts = delta.receipts();
    assert(receipts.size() == 1);
    assert(receipts.front().schema == session_semantic_graph_schema);
    assert(receipts.front().episode_id == source->derivative.episode_id);
    assert(receipts.front().member_edge_start == 7);
    assert(receipts.front().member_edge_count == 11);
    assert(receipts.front().interpretation == source->interpretation_receipt);
    assert(receipts.front().new_observation_count() == 0);
    assert(receipts.front().independent_evidence_count() == 0);
    assert(!receipts.front().grants_authority());

    delta.require_parent(index, "memory-1", "vrs-1", 7);
    assert(rejects([&] { delta.require_parent(index, "stale", "vrs-1", 7); }));
    assert(rejects([&] { delta.require_parent(index, "memory-1", "vrs-2", 7); }));
    assert(rejects([&] { delta.require_parent(index, "memory-1", "vrs-1", 8); }));
    assert(rejects([&] {
        delta.require_parent(TermAddressIndex::build(
            {"parent:0", "parent:1", "parent:2", "unrelated"}),
            "memory-1", "vrs-1", 7);
    }));
}

void test_model_replacement_reuses_document_anchor_and_unit_addresses() {
    const auto index = parent_index();
    const auto first = prepare_session_semantic_delta(
        binding("session-semantic:model-a"), index, 0, "memory-1", "vrs-1");
    const auto second = prepare_session_semantic_delta(
        binding("session-semantic:model-b"), index, 0, "memory-1", "vrs-1");
    auto left = first.appended_terms;
    auto right = second.appended_terms;
    std::erase(left, "session-semantic:model-a");
    std::erase(right, "session-semantic:model-b");
    assert(left == right);
}

void test_missing_parent_duplicate_derivative_and_malformed_binding_reject() {
    const auto source = binding();
    assert(rejects([&] {
        (void)prepare_session_semantic_delta(
            source, TermAddressIndex::build({"parent:0", "parent:1"}),
            0, "memory-1", "vrs-1");
    }));
    assert(rejects([&] {
        (void)prepare_session_semantic_delta(
            source, TermAddressIndex::build({"parent:0", "parent:1", "parent:2",
                                             source->derivative.episode_id}),
            0, "memory-1", "vrs-1");
    }));
    assert(rejects([&] {
        (void)prepare_session_semantic_delta(
            source, parent_index(), 0, "stale", "vrs-1");
    }));

    auto malformed = *source;
    malformed.units.front().anchors.clear();
    assert(rejects([&] {
        (void)prepare_session_semantic_delta(
            std::make_shared<const BoundSessionSemantics>(std::move(malformed)),
            parent_index(), 0, "memory-1", "vrs-1");
    }));
}

void test_large_parent_is_never_materialized_or_replaced() {
    constexpr std::size_t count = 100000;
    std::vector<std::string> terms{"parent:0", "parent:1", "parent:2"};
    terms.reserve(count + 3);
    for (std::size_t index = 0; index < count; ++index)
        terms.push_back("cold:" + std::to_string(index));
    const auto parent = TermAddressIndex::build(std::move(terms));
    const auto cold = parent->terms()->cold_base();
    const auto delta = prepare_session_semantic_delta(
        binding(), parent, count, "memory-1", "vrs-1");
    assert(delta.address_index.get() == parent.get());
    assert(delta.address_index->terms()->cold_base() == cold);
    assert(delta.appended_terms.size() == 6);
    assert(parent->size() == count + 3);
    assert(parent->lookup("cold:90000").has_value());
}

void test_python_float_spelling_in_semantic_payloads() {
    assert(semantic_canonical_json(JsonValue(1.2345678901234568e16)) ==
           "1.2345678901234568e+16");
    assert(semantic_canonical_json(JsonValue(1e-4)) == "0.0001");
}

}  // namespace

int main() {
    test_python_address_parity();
    test_sparse_session_graph_preserves_all_parents_and_roles();
    test_model_replacement_reuses_document_anchor_and_unit_addresses();
    test_missing_parent_duplicate_derivative_and_malformed_binding_reject();
    test_large_parent_is_never_materialized_or_replaced();
    test_python_float_spelling_in_semantic_payloads();
    std::cout << "PASS session semantic binding: Python address parity, sparse parent lineage, "
                 "generation guards and 100k parent sharing\n";
}
