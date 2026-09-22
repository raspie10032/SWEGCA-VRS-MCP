#include "native_graph_numeric_record.hpp"

#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>

#include <zlib.h>

namespace swegca::vrs {
namespace {

static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559,
              "SWEGCA numerical records require IEEE-754 float32");

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:26-29
void put_u32(std::byte* out, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        out[i] = static_cast<std::byte>((value >> (i * 8)) & 0xff);
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:26-29
std::uint32_t get_u32(const std::byte* bytes) {
    std::uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i)
        value |= static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[i])) << (i * 8);
    return value;
}

// SWEGCA: src/swegca_vrs2/native_journal.py@c06092a:1-10
std::uint32_t record_crc(const std::byte* bytes, std::size_t size) {
    return crc32(0, reinterpret_cast<const Bytef*>(bytes), static_cast<uInt>(size));
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_delta.py@7536139:59-66
void require_node(GraphNumericNodeRecord value) {
    if (!std::isfinite(value.direct) || !std::isfinite(value.score))
        throw std::runtime_error("graph_numeric_node_invalid");
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_delta.py@7536139:145-155
void require_edge(GraphNumericEdgeRecord value) {
    if ((value.edge.sign != -1 && value.edge.sign != 0 && value.edge.sign != 1) ||
        !std::isfinite(value.edge.vrs_strength) ||
        value.edge.vrs_strength < 0 || !std::isfinite(value.strength) ||
        value.strength < 0)
        throw std::runtime_error("graph_numeric_edge_invalid");
}

}  // namespace

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_delta.py@7536139:141-163
std::array<std::byte, graph_numeric_node_record_bytes>
encode_graph_numeric_node(GraphNumericNodeRecord value) {
    require_node(value);
    std::array<std::byte, graph_numeric_node_record_bytes> bytes{};
    put_u32(bytes.data(), std::bit_cast<std::uint32_t>(value.direct));
    put_u32(bytes.data() + 4, std::bit_cast<std::uint32_t>(value.score));
    bytes[8] = static_cast<std::byte>(value.unresolved ? 1 : 0);
    put_u32(bytes.data() + 12, record_crc(bytes.data(), 12));
    return bytes;
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_delta.py@7536139:141-163
GraphNumericNodeRecord decode_graph_numeric_node(
    std::span<const std::byte, graph_numeric_node_record_bytes> bytes) {
    if (bytes[9] != std::byte{} || bytes[10] != std::byte{} ||
        bytes[11] != std::byte{} ||
        (bytes[8] != std::byte{} && bytes[8] != std::byte{1}) ||
        get_u32(bytes.data() + 12) != record_crc(bytes.data(), 12))
        throw std::runtime_error("graph_numeric_node_corrupt");
    GraphNumericNodeRecord value{
        std::bit_cast<float>(get_u32(bytes.data())),
        std::bit_cast<float>(get_u32(bytes.data() + 4)),
        bytes[8] == std::byte{1}};
    require_node(value);
    return value;
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_delta.py@7536139:157-163
std::array<std::byte, graph_numeric_edge_record_bytes>
encode_graph_numeric_edge(GraphNumericEdgeRecord value) {
    require_edge(value);
    std::array<std::byte, graph_numeric_edge_record_bytes> bytes{};
    put_u32(bytes.data(), value.edge.source);
    put_u32(bytes.data() + 4, value.edge.target);
    bytes[8] = static_cast<std::byte>(
        value.edge.sign == -1 ? 0xff : static_cast<unsigned char>(value.edge.sign));
    put_u32(bytes.data() + 12,
            std::bit_cast<std::uint32_t>(value.edge.vrs_strength));
    put_u32(bytes.data() + 16, std::bit_cast<std::uint32_t>(value.strength));
    put_u32(bytes.data() + 20, record_crc(bytes.data(), 20));
    return bytes;
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_delta.py@7536139:157-163
GraphNumericEdgeRecord decode_graph_numeric_edge(
    std::span<const std::byte, graph_numeric_edge_record_bytes> bytes) {
    const auto sign = std::to_integer<unsigned char>(bytes[8]);
    if (bytes[9] != std::byte{} || bytes[10] != std::byte{} ||
        bytes[11] != std::byte{} ||
        (sign != 0xff && sign != 0 && sign != 1) ||
        get_u32(bytes.data() + 20) != record_crc(bytes.data(), 20))
        throw std::runtime_error("graph_numeric_edge_corrupt");
    GraphNumericEdgeRecord value{
        EventEdge{get_u32(bytes.data()), get_u32(bytes.data() + 4),
                  sign == 0xff ? std::int8_t{-1} : static_cast<std::int8_t>(sign),
                  std::bit_cast<float>(get_u32(bytes.data() + 12))},
        std::bit_cast<float>(get_u32(bytes.data() + 16))};
    require_edge(value);
    return value;
}

}  // namespace swegca::vrs
