#include "world/vrs_event_delta.hpp"

#include <array>
#include <bit>
#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <vector>

using namespace swegca::world;

namespace {

std::shared_ptr<const EventSignalInputs> base() {
    return std::make_shared<const EventSignalInputs>(
        std::string(64, 'a'), std::vector<float>{0.1F, 0.2F, 0.3F},
        std::vector<float>{-0.1F, 0.4F, -0.5F},
        std::vector<EventSignalEdge>{{0, 1, 1, 1.0F}, {1, 2, -1, 0.75F}},
        std::vector<float>{1.0F, 0.75F}, std::vector<std::uint8_t>{0, 0, 1});
}

bool rejects(const auto& operation) {
    try { operation(); }
    catch (const std::invalid_argument&) { return true; }
    return false;
}

void shared_delta_matches_dense() {
    const auto parent = base();
    const std::array<float, 2> appended_direct{0.6F, -0.4F};
    const std::array<float, 2> appended_score{0.0F, 0.2F};
    const std::array<std::uint8_t, 2> appended_unresolved{0, 1};
    const std::array<EventSignalEdge, 2> appended_edges{
        EventSignalEdge{2, 3, 1, 0.5F}, EventSignalEdge{4, 1, -1, 0.25F}};
    const std::array<float, 2> appended_strength{0.5F, 0.25F};
    const std::array<std::size_t, 1> base_indices{1};
    const std::array<float, 1> base_values{0.8F};
    const std::array<std::size_t, 1> strength_indices{0};
    const std::array<float, 1> strength_values{1.125F};
    const std::array<std::size_t, 1> direct_indices{1};
    const std::array<float, 1> direct_values{-0.2F};
    const std::array<std::size_t, 1> score_indices{2};
    const std::array<float, 1> score_values{-0.25F};
    const std::array<std::size_t, 1> unresolved_indices{2};
    const std::array<std::uint8_t, 1> unresolved_values{0};

    const auto delta = prepare_event_delta(
        parent, std::string(64, 'b'), appended_direct, appended_score,
        appended_unresolved, appended_edges, appended_strength,
        base_indices, base_values, strength_indices, strength_values,
        direct_indices, direct_values, score_indices, score_values,
        unresolved_indices, unresolved_values);
    assert(delta->delta_parent() == parent.get());
    assert(delta->score.size() == 5 && delta->edges.size() == 4);
    assert(delta->direct[1] == -0.2F && delta->direct[3] == 0.6F);
    assert(delta->score[2] == -0.25F && delta->score[4] == 0.2F);
    assert(delta->unresolved[2] == 0 && delta->unresolved[4] == 1);
    assert(delta->strength[0] == 1.125F && delta->strength[3] == 0.25F);
    assert(delta->edges[1].vrs_strength == 0.8F);
    assert(delta->edges[2] == appended_edges[0] && delta->edges[3] == appended_edges[1]);
    assert(delta->incoming(1) == std::vector<std::size_t>({0, 3}));
    assert(delta->outgoing(2) == std::vector<std::size_t>({2}));
    assert(parent->score.size() == 3 && parent->edges.size() == 2);
    assert(parent->direct[1] == 0.2F && parent->score[2] == -0.5F &&
           parent->strength[0] == 1.0F && parent->unresolved[2] == 1);

    const auto dense = std::make_shared<const EventSignalInputs>(
        std::string(64, 'b'), delta->direct.materialize(), delta->score.materialize(),
        delta->edges.materialize(), delta->strength.materialize(),
        delta->unresolved.materialize());
    const std::array<std::size_t, 4> seeds{1, 2, 3, 4};
    const auto sparse_result = settle_event_signal(*delta, seeds, {}, "vrs-edge:", nullptr, 1000);
    const auto dense_result = settle_event_signal(*dense, seeds, {}, "vrs-edge:", nullptr, 1000);
    assert(sparse_result.scores == dense_result.scores);
    assert(sparse_result.strengths == dense_result.strengths);
    assert(sparse_result.pending_nodes == dense_result.pending_nodes);
    assert(sparse_result.rounds == dense_result.rounds);
    assert(sparse_result.node_evaluations == dense_result.node_evaluations);
    assert(sparse_result.edge_evaluations == dense_result.edge_evaluations);

    const std::array<std::size_t, 1> second_score_indices{4};
    const std::array<float, 1> second_score_values{0.9F};
    const auto second = prepare_event_delta(
        delta, std::string(64, 'c'), {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {},
        second_score_indices, second_score_values);
    assert(second->score[4] == 0.9F && delta->score[4] == 0.2F &&
           parent->score.size() == 3);
    assert(second->edges[3] == appended_edges[1]);
}

void invalid_delta_is_rejected() {
    const auto parent = base();
    const std::array<float, 1> one_float{0.1F};
    const std::array<std::uint8_t, 1> one_flag{0};
    const std::array<EventSignalEdge, 1> bad_endpoint{EventSignalEdge{0, 9, 1, 1.0F}};
    assert(rejects([&] {
        (void)prepare_event_delta(parent, std::string(64, 'b'), one_float, {},
                                  one_flag, {}, {});
    }));
    assert(rejects([&] {
        (void)prepare_event_delta(parent, std::string(64, 'b'), one_float, one_float,
                                  one_flag, bad_endpoint, one_float);
    }));
    const std::array<std::size_t, 2> duplicate{0, 0};
    const std::array<float, 2> duplicate_values{0.1F, 0.2F};
    assert(rejects([&] {
        (void)prepare_event_delta(parent, std::string(64, 'b'), {}, {}, {}, {}, {},
                                  {}, {}, duplicate, duplicate_values);
    }));
    const std::array<float, 1> negative{-0.1F};
    const std::array<std::size_t, 1> first{0};
    assert(rejects([&] {
        (void)prepare_event_delta(parent, std::string(64, 'b'), {}, {}, {}, {}, {},
                                  {}, {}, first, negative);
    }));
    const std::array<float, 1> nan{std::numeric_limits<float>::quiet_NaN()};
    assert(rejects([&] {
        (void)prepare_event_delta(parent, std::string(64, 'b'), nan, nan, one_flag,
                                  {}, {});
    }));
}

void dependency_segments_remain_geometrically_bounded() {
    auto current = base();
    const std::array<EventSignalEdge, 1> edge{EventSignalEdge{0, 1, 1, 0.5F}};
    const std::array<float, 1> strength{0.5F};
    for (std::size_t iteration = 0; iteration < 256; ++iteration) {
        const char digit = "bcdef0123456789a"[iteration % 16];
        current = prepare_event_delta(
            current, std::string(64, digit), {}, {}, {}, edge, strength);
    }
    assert(current->dependency_segment_count() <= 16);
    assert(current->dependency_index_bytes() == current->edges.size() * 16);
    const auto outgoing = current->outgoing(0);
    const auto incoming = current->incoming(1);
    assert(outgoing.size() == 257 && incoming.size() == 257);
    for (std::size_t index = 0; index < outgoing.size(); ++index) {
        const auto expected = index == 0 ? 0 : index + 1;
        assert(outgoing[index] == expected);
        assert(incoming[index] == expected);
    }
}

}  // namespace

int main() {
    assert(vrs_event_delta_source_sha256 ==
           "ab60dccb5914d6eb01319c61094d1e43f1e883da180c4e815a7201f99cc9cfdd");
    assert(vrs_event_dependency_source_sha256 ==
           "db3eac9ac08e70dbb6afdf4d9ccd6df61fe0d9f50148959c91e6d69b70ddc225");
    shared_delta_matches_dense();
    invalid_delta_is_rejected();
    dependency_segments_remain_geometrically_bounded();
    std::cout << "VRS sparse event delta tests passed\n";
}
