#include "checkpoint/materialized_tensor.hpp"

#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

using namespace swegca::checkpoint;

namespace {
template <class Function>
bool rejects(Function&& function) {
    try { function(); } catch (const std::exception&) { return true; }
    return false;
}
}  // namespace

int main() {
    const MaterializedTensor half(
        TensorDType::float16, {2},
        {std::byte{0x00}, std::byte{0x3c}, std::byte{0x00}, std::byte{0xc0}});
    assert(half.value_as_double(0) == 1.0 && half.value_as_double(1) == -2.0);
    const MaterializedTensor bfloat(
        TensorDType::bfloat16, {1}, {std::byte{0x80}, std::byte{0x3f}});
    assert(bfloat.value_as_double(0) == 1.0);
    const MaterializedTensor integer(
        TensorDType::int64, {2},
        {std::byte{0xff}, std::byte{0xff}, std::byte{0xff}, std::byte{0xff},
         std::byte{0xff}, std::byte{0xff}, std::byte{0xff}, std::byte{0x7f},
         std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
         std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x80}});
    const auto integers = integer.int64_values();
    assert(integers[0] == std::numeric_limits<std::int64_t>::max());
    assert(integers[1] == std::numeric_limits<std::int64_t>::min());
    assert(rejects([&] { (void)integer.value_as_double(0); }));
    const MaterializedTensor scalar(TensorDType::float32, {},
                                    {std::byte{0x00}, std::byte{0x00},
                                     std::byte{0x80}, std::byte{0x3f}});
    assert(scalar.numel() == 1 && scalar.value_as_double(0) == 1.0);
    const MaterializedTensor empty(TensorDType::float32, {2, 0, 3}, {});
    assert(empty.numel() == 0 && empty.bytes().empty());
    const MaterializedTensor late_empty(
        TensorDType::float32, {std::numeric_limits<std::uint64_t>::max(), 2, 0}, {});
    assert(late_empty.numel() == 0);

    const auto& profile = checkpoint_profiles().front();
    auto checkpoint = RestrictedCheckpoint::open(profile.original_path);
    const auto queries = materialize_model_tensor(checkpoint, "to_world.world_queries");
    assert(queries.dtype() == TensorDType::float32);
    assert((std::vector<std::uint64_t>(queries.shape().begin(), queries.shape().end()) ==
            std::vector<std::uint64_t>{32, 256}));
    assert(queries.numel() == 8192 && queries.bytes().size() == 32768);
    const auto query_values = queries.float32_values();
    for (const auto value : query_values) assert(std::isfinite(value));
    assert(static_cast<float>(queries.value_as_double(17)) == query_values[17]);

    const auto& descriptor = checkpoint.manifest().tensors.front();
    const auto teacher = materialize_tensor(checkpoint, descriptor);
    const auto storage = checkpoint.read_storage(descriptor.storage_key);
    const auto item_size = dtype_size(descriptor.dtype);
    const auto logical_offset = (std::uint64_t{123} * 256 + 77) * item_size;
    const auto storage_offset = (std::uint64_t{123} + std::uint64_t{77} * 512) * item_size;
    for (std::uint64_t byte = 0; byte != item_size; ++byte) {
        assert(teacher.bytes()[logical_offset + byte] == storage[storage_offset + byte]);
    }
    for (std::uint64_t row = 0; row != 512; ++row) {
        for (std::uint64_t column = 0; column != 256; ++column) {
            const auto logical = (row * 256 + column) * item_size;
            const auto physical = (row + column * 512) * item_size;
            for (std::uint64_t byte = 0; byte != item_size; ++byte) {
                assert(teacher.bytes()[logical + byte] == storage[physical + byte]);
            }
        }
    }

    assert(rejects([&] { (void)materialize_model_tensor(checkpoint, "missing.tensor"); }));
    assert(rejects([&] { (void)queries.value_as_double(queries.numel()); }));
    auto forged = *checkpoint.manifest().find_model_tensor("to_world.world_queries");
    forged.logical_bytes += 4;
    assert(rejects([&] { (void)materialize_tensor(checkpoint, forged); }));
    std::cout << "materialized checkpoint tensor tests passed\n";
}
