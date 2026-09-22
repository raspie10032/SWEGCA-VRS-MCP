#pragma once

#include "event_vrs_inputs.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace swegca::vrs {

// Physical records carry the author's separate direct/score/unresolved node
// vectors and endpoint/sign/base/current-strength edge fields. Their layout
// is explicit little-endian bytes, independent of C++ struct padding.
// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:34-76
// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_delta.py@7536139:117-180
struct GraphNumericNodeRecord {
    float direct;
    float score;
    bool unresolved;
};

struct GraphNumericEdgeRecord {
    EventEdge edge;
    float strength;
};

inline constexpr std::size_t graph_numeric_node_record_bytes = 16;
inline constexpr std::size_t graph_numeric_edge_record_bytes = 24;

// Source values are already float32; copying their bits here must not change
// the settlement order or round them a second time.
// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_delta.py@7536139:141-163
[[nodiscard]] std::array<std::byte, graph_numeric_node_record_bytes>
encode_graph_numeric_node(GraphNumericNodeRecord value);

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_delta.py@7536139:141-163
[[nodiscard]] GraphNumericNodeRecord decode_graph_numeric_node(
    std::span<const std::byte, graph_numeric_node_record_bytes> bytes);

// The edge's vrs_strength is its base. Current strength remains separate.
// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_delta.py@7536139:157-163
[[nodiscard]] std::array<std::byte, graph_numeric_edge_record_bytes>
encode_graph_numeric_edge(GraphNumericEdgeRecord value);

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_delta.py@7536139:157-163
[[nodiscard]] GraphNumericEdgeRecord decode_graph_numeric_edge(
    std::span<const std::byte, graph_numeric_edge_record_bytes> bytes);

}  // namespace swegca::vrs
