#include "world/vrs_edge_address_index.hpp"
#include "world/vrs_event_delta.hpp"

#include <array>
#include <cassert>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

using namespace swegca::world;

namespace {

bool rejects(const auto& operation) {
    try { operation(); }
    catch (const std::invalid_argument&) { return true; }
    return false;
}

std::shared_ptr<const EventSignalInputs> fixture() {
    return std::make_shared<const EventSignalInputs>(
        std::string(64, 'a'), std::vector<float>{0.0F, 0.1F, -0.1F, 0.2F},
        std::vector<float>{0.0F, 0.2F, -0.2F, 0.4F},
        std::vector<EventSignalEdge>{
            {2, 1, -1, 0.75F}, {0, 1, 1, 1.0F}, {2, 3, 1, 0.5F}},
        std::vector<float>{0.75F, 1.0F, 0.5F},
        std::vector<std::uint8_t>{0, 0, 0, 0});
}

void lookup_strength_rebind_and_append_are_exact() {
    const auto parent = fixture();
    const auto index = CanonicalEdgeAddressIndex::build(parent->edges);
    index->require_source(parent->edges);
    assert(index->segment_count() == 1);
    assert(index->index_bytes() == 3 * 13);
    assert(index->lookup(2, 1, -1) == 0);
    assert(index->lookup(0, 1, 1) == 1);
    assert(index->lookup(2, 3, 1) == 2);
    assert(!index->lookup(3, 2, 1));

    const std::array<std::size_t, 1> base_index{0};
    const std::array<float, 1> base_value{0.625F};
    const auto strength_only = prepare_event_delta(
        parent, std::string(64, 'b'), {}, {}, {}, {}, {},
        base_index, base_value);
    const auto rebound = index->advance_event_delta(*parent, *strength_only);
    assert(rebound->segment_count() == 1);
    assert(rebound->lookup(2, 1, -1) == 0);
    assert(strength_only->edges[0].vrs_strength == 0.625F);

    const std::array<EventSignalEdge, 1> edge{EventSignalEdge{3, 0, -1, 0.25F}};
    const std::array<float, 1> strength{0.25F};
    const auto successor = prepare_event_delta(
        strength_only, std::string(64, 'c'), {}, {}, {}, edge, strength);
    const auto extended = rebound->advance_event_delta(*strength_only, *successor);
    assert(extended->segment_count() == 2);
    assert(extended->index_bytes() == 4 * 13);
    assert(extended->lookup(3, 0, -1) == 3);
    assert(extended->lookup(2, 1, -1) == 0);

    const auto generic = index->advance(successor->edges);
    assert(generic->lookup(3, 0, -1) == 3);
    assert(generic->index_bytes() == extended->index_bytes());
}

void wrong_generation_and_duplicate_addresses_are_rejected() {
    const auto parent = fixture();
    const auto index = CanonicalEdgeAddressIndex::build(parent->edges);
    const auto foreign = fixture();
    assert(rejects([&] { index->require_source(foreign->edges); }));
    assert(rejects([&] {
        (void)index->lookup(
            std::uint64_t{1} << 32U, 0, 1);
    }));
    assert(rejects([&] { (void)index->lookup(0, 0, 128); }));

    const std::array<EventSignalEdge, 1> repeated{EventSignalEdge{0, 1, 1, 0.5F}};
    const std::array<float, 1> strength{0.5F};
    const auto duplicate = prepare_event_delta(
        parent, std::string(64, 'b'), {}, {}, {}, repeated, strength);
    assert(rejects([&] {
        (void)index->advance_event_delta(*parent, *duplicate);
    }));

    const std::array<EventSignalEdge, 2> repeated_pair{
        EventSignalEdge{3, 0, 1, 0.5F}, EventSignalEdge{3, 0, 1, 0.25F}};
    const std::array<float, 2> pair_strength{0.5F, 0.25F};
    const auto duplicates = prepare_event_delta(
        parent, std::string(64, 'c'), {}, {}, {}, repeated_pair, pair_strength);
    assert(rejects([&] {
        (void)index->advance_event_delta(*parent, *duplicates);
    }));

    const auto foreign_delta = prepare_event_delta(
        foreign, std::string(64, 'd'), {}, {}, {}, {}, {});
    assert(rejects([&] {
        (void)index->advance_event_delta(*parent, *foreign_delta);
    }));

    auto changed = parent->edges.materialize();
    changed[0].source = 1;
    const PersistentEventVector<EventSignalEdge> changed_prefix(std::move(changed));
    assert(rejects([&] { (void)index->advance(changed_prefix); }));

    const PersistentEventVector<EventSignalEdge> duplicate_parent(
        std::vector<EventSignalEdge>{{0, 1, 1, 1.0F}, {0, 1, 1, 0.5F}});
    assert(rejects([&] {
        (void)CanonicalEdgeAddressIndex::build(duplicate_parent);
    }));
}

void delta_segments_stay_geometrically_bounded() {
    constexpr std::size_t nodes = 300;
    auto parent = std::make_shared<const EventSignalInputs>(
        std::string(64, 'a'), std::vector<float>(nodes), std::vector<float>(nodes),
        std::vector<EventSignalEdge>{}, std::vector<float>{},
        std::vector<std::uint8_t>(nodes));
    auto index = CanonicalEdgeAddressIndex::build(parent->edges);
    const std::array<float, 1> strength{0.5F};
    for (std::size_t ordinal = 0; ordinal < 256; ++ordinal) {
        const std::array<EventSignalEdge, 1> edge{
            EventSignalEdge{static_cast<std::uint32_t>(ordinal), 299, 1, 0.5F}};
        const char digit = "bcdef0123456789a"[ordinal % 16];
        const auto successor = prepare_event_delta(
            parent, std::string(64, digit), {}, {}, {}, edge, strength);
        index = index->advance_event_delta(*parent, *successor);
        parent = successor;
    }
    assert(index->segment_count() <= 16);
    assert(index->index_bytes() == 256 * 13);
    for (std::size_t ordinal = 0; ordinal < 256; ++ordinal)
        assert(index->lookup(ordinal, 299, 1) == ordinal);
}

}  // namespace

int main() {
    assert(vrs_edge_address_index_source_sha256 ==
           "7edceca917194419828845d7350f7b19a2747b19fbddd1a7ccfbf07a2be6cf56");
    lookup_strength_rebind_and_append_are_exact();
    wrong_generation_and_duplicate_addresses_are_rejected();
    delta_segments_stay_geometrically_bounded();
    std::cout << "VRS canonical edge address index tests passed\n";
}
