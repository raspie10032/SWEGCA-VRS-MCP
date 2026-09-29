#include "world/sensor_definition.hpp"

#include <array>
#include <cassert>
#include <iostream>
#include <string>
#include <vector>

using namespace swegca::world;

int main() {
    const std::array<ContinuousSensorEvent, 2> events{{
        {0, "t0", "sensor:0", "context:0", "테스트", 1.0, "게임 시작", "첫 발화"},
        {1, "t1", "sensor:1", "context:1", "테스트", 1.0, "게임 시작", "첫 발화"},
    }};
    const std::array<std::size_t, 2> indices{0, 1};
    const auto candidate = make_sensor_definition_candidate(
        "screen_ocr", "게임", indices, events);
    assert(candidate.candidate_id ==
           "sensor-definition:2ab0224e7789a8fc1349c8abb5ad68491236c76f2ac9e007baf44427db822bd2");
    assert((candidate.source_event_content_hashes == std::vector<std::string>{
        "456399f01325bf038a92e5379a7baa26b2725452af7a94ca063faf07b6f59586",
        "58e6c97992612ccb5cd79a01dd4f0489c4ecf2b1cefdd4af953c9477bdf7c2b3"}));
    assert(candidate.definition_status == DefinitionStatus::partial);
    assert(!candidate.world_write_allowed);
    assert(candidate.unresolved_definitions.size() == 4);
    assert(candidate.counterfactual_plans.size() == 3);
    assert(candidate.counterfactual_plans[0].plan_id ==
           "counterfactual-plan:5a39bca371bb1e4c6fa64708b244f164c89f8a4c23cc472d20d4057b2faa502f");
    assert(candidate.counterfactual_plans[1].plan_id ==
           "counterfactual-plan:5463adfe46a4ce905f97b05045bdf7b051e07b8670b95c86baba344f1029381d");
    assert(candidate.counterfactual_plans[2].plan_id ==
           "counterfactual-plan:d7b4dd09feba46c23e5db30ef9ee4730a841022f3b8a52353cdeceb0fc3766e0");

    const auto observations = candidate_insufficient_observations(
        candidate, "natural-stream");
    assert(observations.size() == 5);
    assert(observations[0].proposal_hash() ==
           "d5b470912f9d24986d378e69729e1d45dfcf102ddddae979b6964c41e5dba5f0");
    assert(observations[1].proposal_hash() ==
           "cb4d3a1a42bcfb8905577fb53c719086532b71ad888dd5fc232176542d44a642");
    assert(observations[2].proposal_hash() ==
           "e4109da4fb5420af65dcd3d01ea6d1f32631a47469e30a8a8634bb4f16a124c3");
    assert(observations[3].proposal_hash() ==
           "f4b2c84b9b195d790c97157feb684f487a9fe5e6b0fe604ca89252052c10909a");
    assert(observations[4].proposal_hash() ==
           "0fd205c05844c7286fdac388090e33d012031004c10941cc881b97004d08a200");
    assert(observations[2].observed_at == 2);
    assert(observations[3].observed_at == 4);
    assert(observations[4].observed_at == 6);
    for (const auto& observation : observations) {
        assert(observation.outcome == "insufficient");
        assert(observation.producer_confidence == 0.0);
    }
    assert(sensor_definition_source_sha256() ==
           "e34715b64e629d4e0dcb58af6becbeec7aabae139e343f7ffc521df1eba72a6a");
    std::cout << "sensor partial definition tests passed\n";
}
