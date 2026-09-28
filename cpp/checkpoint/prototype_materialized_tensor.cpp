#include "checkpoint/prototype_materialized_tensor.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace swegca::checkpoint {

MaterializedTensor materialize_prototype_tensor(
    const PrototypeCheckpoint& checkpoint, const CheckpointTensor& tensor) {
    const auto bound = std::find_if(
        checkpoint.manifest().tensors.begin(), checkpoint.manifest().tensors.end(),
        [&](const CheckpointTensor& candidate) { return &candidate == &tensor; });
    if (bound == checkpoint.manifest().tensors.end()) {
        throw std::invalid_argument(
            "Prototype0 tensor descriptor is not bound to this checkpoint");
    }
    const TensorLayout layout{tensor.dtype, tensor.shape, tensor.stride,
                              tensor.storage_offset_elements, tensor.storage_elements};
    const auto checked = check_tensor_span(layout);
    if (!checked || checked.numel != tensor.numel ||
        checked.logical_bytes != tensor.logical_bytes ||
        checked.storage_span_start_byte != tensor.storage_span_start_byte ||
        checked.storage_span_end_byte_exclusive != tensor.storage_span_end_byte_exclusive ||
        checked.contiguous != tensor.contiguous || !tensor.contiguous) {
        throw std::invalid_argument("Prototype0 tensor descriptor is inconsistent");
    }
    const auto storage = checkpoint.read_storage(tensor.storage_key);
    const auto item_size = dtype_size(tensor.dtype);
    if (item_size == 0 ||
        tensor.storage_elements > std::numeric_limits<std::uint64_t>::max() / item_size ||
        storage.size() != tensor.storage_elements * item_size ||
        tensor.logical_bytes > std::numeric_limits<std::size_t>::max()) {
        throw std::runtime_error("Prototype0 tensor storage size mismatch");
    }
    std::vector<std::byte> logical(static_cast<std::size_t>(tensor.logical_bytes));
    if (!logical.empty()) {
        std::copy_n(storage.data() + tensor.storage_span_start_byte, logical.size(),
                    logical.data());
    }
    return MaterializedTensor(tensor.dtype, tensor.shape, std::move(logical));
}

MaterializedTensor materialize_prototype_tensor(
    const PrototypeCheckpoint& checkpoint, const std::string_view state_dict_key) {
    const auto* tensor = checkpoint.manifest().find_tensor(state_dict_key);
    if (tensor == nullptr) {
        throw std::out_of_range("Prototype0 tensor is missing");
    }
    return materialize_prototype_tensor(checkpoint, *tensor);
}

}  // namespace swegca::checkpoint
