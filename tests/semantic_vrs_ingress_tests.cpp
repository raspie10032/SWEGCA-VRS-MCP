#include "world/semantic_vrs_ingress.hpp"

#include <cassert>
#include <iostream>
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

SemanticAnchor anchor() {
    return {"item", 0, {std::string("source_item_id")}, "text", "original",
            {0, 5}, {}, {}};
}

SemanticMeaningUnit unit() {
    return {"source item", "recorded identifier", "alpha", "affirmed", "reported",
            {"item"}, {}, "unspecified"};
}

SemanticEncoding proposal(const SemanticSourceEpisode& original,
                          std::string model = "worker-one") {
    return {
        original.episode_id,
        original.revision,
        semantic_source_digest(original),
        original.source_addresses,
        {original.steps.front().outcome},
        std::move(model),
        {anchor()},
        {unit()},
        {},
        {},
        {},
    };
}

void test_python_canonical_address_fixture() {
    const auto original = source();
    const auto encoding = proposal(original);
    assert(semantic_source_digest(original) ==
           "32170ea6b37b424d05b0ba6f8bc29a834a8456f16b7c0eb345bc6e9d9585ab16");
    assert(semantic_anchor_address(original.episode_id, original.revision,
                                   encoding.anchors.front()) ==
           "semantic-anchor:72ef6ef756d00675d5089c4115a6207760bef9bdeb9f18dd7bd3093a40392add");
    assert(semantic_unit_address(original.episode_id, original.revision,
                                 encoding.units.front()) ==
           "semantic-unit:a015ee74627299914b4fb9b4a740a97175843f51c9c92eace718f17935cde946");
    assert(semantic_encoding_episode_id(encoding) ==
           "semantic-encoding:e2c0b79bbfa7b232e8f7bd307ea94d930d5c3803a9ec8ece2fc38d0cbfeb8030");

    const auto coordinates = prepare_claim_graph_addresses(
        encoding, semantic_encoding_episode_id(encoding));
    assert(coordinates.size() == 1 && coordinates.front().size() == 4);
    assert(coordinates.front()[0].kind == "source_derivation");
    assert(coordinates.front()[1].kind == "encoding_unit");
    assert(coordinates.front()[2].kind == "unit_anchor_proposal");
    assert(coordinates.front()[3].kind == "anchor_source_location");
}

void test_python_float_unicode_and_qualifier_address_fixtures() {
    const auto make = [](const double value) {
        return SemanticMeaningUnit{
            "측정", "값", value, "unknown", "inferred", {"item"},
            {{"condition", "관측 시", {"item"}}}, "literal"};
    };
    const std::vector<std::pair<double, std::string>> fixtures{
        {1e-7, "semantic-unit:3e6980d4d3e15e6232c66e7d9777eced0e16f9b8c4460a5a8aba8a8def568191"},
        {1e-4, "semantic-unit:245927b9fb224628b9fd3d793c902a317696e60a2345bbeaf9d4c68c652b2468"},
        {1e15, "semantic-unit:0791fb79ff1267a12000fcb343c8a18aebd52a87543435c5a93e6274a58d565c"},
        {1e16, "semantic-unit:32fa1f2dc1813f457f07bb4df39e603cace1a69996f7dab52e194cf49ccc4907"},
        {-0.0, "semantic-unit:fe54ce44879495e058ff113c826457ef0c8b9a5faf5b6290dea29e8333ed940e"},
        {1.0, "semantic-unit:fc3ecae3399ea7442e1c9a065ea5a9544f7036f83f2eb8aaa8f6a783d14c34af"},
    };
    for (const auto& [value, expected] : fixtures)
        assert(semantic_unit_address("source:alpha", "revision:1", make(value)) == expected);

    const auto original = source();
    auto encoding = proposal(original);
    encoding.units.front().qualifiers = {{"condition", "only if observed", {"item"}}};
    const auto index = TermAddressIndex::build({original.episode_id});
    const auto delta = prepare_semantic_graph_delta(index, 0, {original}, {encoding});
    // The unit and its qualifier reference the same anchor. dict.fromkeys in
    // the Python source makes this one unit-anchor association, not two.
    assert(delta.edge_rows.size() == 4);
    assert(delta.receipts().front().edge_roles.back().kind == "unit_anchor_proposal");
}

void test_delta_has_exact_directed_associations_and_no_authority() {
    const auto original = source();
    const auto encoding = proposal(original);
    const auto index = TermAddressIndex::build({original.episode_id, "unrelated"});
    const auto delta = prepare_semantic_graph_delta(index, 2, {original}, {encoding});
    assert(delta.address_index.get() == index.get());
    assert(delta.edge_start == 2);
    assert(delta.appended_terms == std::vector<std::string>({
        semantic_encoding_episode_id(encoding),
        semantic_anchor_address(original.episode_id, original.revision, encoding.anchors[0]),
        semantic_unit_address(original.episode_id, original.revision, encoding.units[0]),
    }));
    assert(delta.edge_rows == std::vector<EventSignalEdge>({
        {0, 2, 1, .75F}, {3, 0, 1, .75F},
        {2, 4, 1, .75F}, {4, 3, 1, .75F},
    }));
    const auto rows = delta.receipts();
    assert(rows.size() == 1);
    const auto& receipt = rows.front();
    assert(receipt.node_id == 2 && receipt.member_edge_start == 2);
    assert((receipt.anchor_nodes ==
            std::vector<std::pair<std::string, std::uint32_t>>({{"item", 3}})));
    assert(receipt.unit_nodes == std::vector<std::uint32_t>{4});
    assert(receipt.edge_roles.size() == 4);
    assert(!receipt.associations_are_logical_implications());
    assert(receipt.independent_evidence_count() == 0);
    assert(receipt.new_observation_count() == 0);
    assert(!receipt.grants_authority());

    auto detached = delta.receipts();
    detached.front().encoding.model = "injected";
    detached.front().anchor_nodes.clear();
    assert(delta.receipts().front().encoding.model == "worker-one");
    assert(delta.receipts().front().anchor_nodes.size() == 1);
    assert(index->size() == 2 && !index->contains(delta.appended_terms.front()));
}

void test_specialists_share_meaning_and_anchor_nodes() {
    const auto original = source();
    const auto first = proposal(original, "worker-one");
    const auto second = proposal(original, "worker-two");
    const auto index = TermAddressIndex::build({original.episode_id});
    const auto delta = prepare_semantic_graph_delta(index, 7, {original}, {first, second});
    assert(delta.appended_terms.size() == 4);
    const auto receipts = delta.receipts();
    assert(receipts.size() == 2);
    assert(receipts[0].episode_id != receipts[1].episode_id);
    assert(receipts[0].node_id != receipts[1].node_id);
    assert(receipts[0].anchor_nodes == receipts[1].anchor_nodes);
    assert(receipts[0].unit_nodes == receipts[1].unit_nodes);
    assert(receipts[0].member_edge_start == 7);
    assert(receipts[1].member_edge_start == 11);
    assert(delta.edge_rows.size() == 8);
    assert(rejects([&] {
        (void)prepare_semantic_graph_delta(index, 0, {original}, {first, first});
    }));

    const auto first_delta = prepare_semantic_graph_delta(index, 0, {original}, {first});
    const auto successor = index->append_shared(first_delta.appended_terms);
    const auto following = prepare_semantic_graph_delta(
        successor, first_delta.edge_rows.size(), {original}, {second});
    assert(following.appended_terms.size() == 1);
    const auto previous_receipt = first_delta.receipts().front();
    const auto following_receipt = following.receipts().front();
    assert(previous_receipt.anchor_nodes == following_receipt.anchor_nodes);
    assert(previous_receipt.unit_nodes == following_receipt.unit_nodes);
}

void test_unresolved_anchor_is_a_typed_association() {
    const auto original = source();
    auto encoding = proposal(original);
    encoding.units.clear();
    encoding.unresolved = {"item"};
    const auto index = TermAddressIndex::build({original.episode_id});
    const auto delta = prepare_semantic_graph_delta(index, 0, {original}, {encoding});
    assert(delta.appended_terms.size() == 2 && delta.edge_rows.size() == 3);
    const auto receipt = delta.receipts().front();
    assert(receipt.unit_nodes.empty());
    assert(receipt.edge_roles.back().kind == "unresolved_anchor");
    assert(receipt.edge_roles.back().anchor_id == "item");
}

void test_source_binding_and_graph_preconditions_fail_closed() {
    const auto original = source();
    const auto valid = proposal(original);
    const auto index = TermAddressIndex::build({original.episode_id});
    auto changed_digest = valid;
    changed_digest.source_digest[0] = '0';
    assert(rejects([&] {
        (void)prepare_semantic_graph_delta(index, 0, {original}, {changed_digest});
    }));
    auto changed_revision = valid;
    changed_revision.source_revision = "revision:other";
    assert(rejects([&] {
        (void)prepare_semantic_graph_delta(index, 0, {original}, {changed_revision});
    }));
    auto bad_anchor = valid;
    bad_anchor.anchors.front().char_range = {0, 999};
    assert(rejects([&] {
        (void)prepare_semantic_graph_delta(index, 0, {original}, {bad_anchor});
    }));
    assert(rejects([&] {
        (void)prepare_semantic_graph_delta(index, 0, {}, {valid});
    }));
    const auto wrong_graph = TermAddressIndex::build({"other"});
    assert(rejects([&] {
        (void)prepare_semantic_graph_delta(wrong_graph, 0, {original}, {valid});
    }));
    assert(rejects([&] {
        (void)prepare_semantic_graph_delta({}, 0, {original}, {valid});
    }));
    assert(index->size() == 1);
}

void test_large_parent_is_shared_and_only_delta_is_appended() {
    const auto original = source();
    std::vector<std::string> terms{original.episode_id};
    terms.reserve(100001);
    for (std::size_t index = 0; index < 100000; ++index)
        terms.push_back("unrelated:" + std::to_string(index));
    const auto parent = TermAddressIndex::build(std::move(terms));
    const auto base_identity = parent->base_dictionary_identity();
    const auto delta = prepare_semantic_graph_delta(
        parent, 100000, {original}, {proposal(original)});
    assert(delta.address_index.get() == parent.get());
    assert(delta.appended_terms.size() == 3 && delta.edge_rows.size() == 4);
    const auto successor = parent->append_shared(delta.appended_terms);
    assert(successor->base_dictionary_identity() == base_identity);
    assert(successor->terms()->cold_base() == parent->terms()->cold_base());
    assert(parent->size() == 100001 && successor->size() == 100004);

    const auto empty = prepare_semantic_graph_delta(parent, 100000, {original}, {});
    assert(empty.address_index.get() == parent.get());
    assert(empty.appended_terms.empty() && empty.edge_rows.empty() &&
           empty.receipts().empty());
}

}  // namespace

int main() {
    test_python_canonical_address_fixture();
    test_python_float_unicode_and_qualifier_address_fixtures();
    test_delta_has_exact_directed_associations_and_no_authority();
    test_specialists_share_meaning_and_anchor_nodes();
    test_unresolved_anchor_is_a_typed_association();
    test_source_binding_and_graph_preconditions_fail_closed();
    test_large_parent_is_shared_and_only_delta_is_appended();
    assert(semantic_vrs_ingress_source_sha256 ==
           "213118a20f197018f3071b1b4fb6cbcfcbdb21265566b338327cddc23cf53f5e");
    std::cout << "semantic VRS ingress tests passed\n";
}
