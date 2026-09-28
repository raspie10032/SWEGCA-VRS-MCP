#pragma once

#include "checkpoint/prototype_checkpoint.hpp"
#include "world/recurrent_cognition.hpp"

#include <string>

namespace swegca::world {

struct PrototypeRecurrentBundle final {
    RecurrentCognitionConfig config;
    RecurrentCognitionWeights weights;
    std::string checkpoint_sha256;
    std::string recurrent_source_sha256;
    std::string source_config_sha256;
    std::string runtime_config_canonical_sha256;
};

// Loads only the exact cognition.* subset from either audited Prototype0
// checkpoint. The complete 38-tensor checkpoint has already been validated by
// PrototypeCheckpoint before this mapping is permitted.
[[nodiscard]] PrototypeRecurrentBundle load_prototype_recurrent_bundle(
    const checkpoint::PrototypeCheckpoint& checkpoint);

}  // namespace swegca::world
