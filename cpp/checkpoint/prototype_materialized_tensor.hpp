#pragma once

#include "checkpoint/materialized_tensor.hpp"
#include "checkpoint/prototype_checkpoint.hpp"

#include <string_view>

namespace swegca::checkpoint {

[[nodiscard]] MaterializedTensor materialize_prototype_tensor(
    const PrototypeCheckpoint& checkpoint, const CheckpointTensor& tensor);
[[nodiscard]] MaterializedTensor materialize_prototype_tensor(
    const PrototypeCheckpoint& checkpoint, std::string_view state_dict_key);

}  // namespace swegca::checkpoint
