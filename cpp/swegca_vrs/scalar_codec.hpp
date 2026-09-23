#pragma once

#include "swegca_vrs/core_imports.hpp"

#include "swegca_vrs/native_tensor.hpp"

#include <cstddef>
#include <span>

// Exact canonical scalar ingress/egress for the native SWEGCA arbiter.
// bf16/f16 widen exactly to binary32; f32 remains binary32; f64 remains
// binary64. Low-precision output is rounded once, ties to even, after the
// arbiter has completed its weighted reduction and world bound.
// Rule: board §4 native tensor value; mosaic_synapse_arbiter.py@5901a5a:263-321.
namespace swegca::vrs {

// SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:263-321
[[nodiscard]] float read_scalar32(ScalarType type, std::span<const std::byte> bytes);
// SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:263-321
[[nodiscard]] double read_scalar64(ScalarType type, std::span<const std::byte> bytes);
// No-throw access into an already owned tensor for the pure arbiter callback.
// False means a type, index, width, or value invariant failed.
// SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:238-262
[[nodiscard]] bool try_read_scalar32(const CognitiveTensor& tensor,
                                     std::size_t element, float& value) noexcept;
// SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:238-262
[[nodiscard]] bool try_read_scalar64(const CognitiveTensor& tensor,
                                     std::size_t element, double& value) noexcept;
// SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:263-321
void write_scalar32(ScalarType type, float value, std::span<std::byte> bytes);
// SWEGCA: src/swegca/mosaic_synapse_arbiter.py@5901a5a:263-321
void write_scalar64(ScalarType type, double value, std::span<std::byte> bytes);

}  // namespace swegca::vrs
