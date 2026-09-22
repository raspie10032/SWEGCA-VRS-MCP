#include "event_signal_strength.hpp"

#include <bit>
#include <charconv>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace swegca::vrs {
namespace {

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_signal.py@7536139:38-50
std::uint32_t round_shift_even(std::uint32_t value, unsigned shift) {
    const auto quotient = value >> shift;
    const auto remainder = value & ((std::uint32_t{1} << shift) - 1);
    const auto halfway = std::uint32_t{1} << (shift - 1);
    return quotient + (remainder > halfway || (remainder == halfway && (quotient & 1)));
}

// IEEE binary16 round-to-nearest-even, matching struct.pack('<e') for finite
// f32 inputs. Values rounding to infinity fail the durable-capacity gate.
// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_signal.py@7536139:43-49
std::uint16_t half_bits(float value) {
    const auto bits = std::bit_cast<std::uint32_t>(value);
    const auto sign = static_cast<std::uint16_t>((bits >> 16) & 0x8000u);
    const auto biased_exponent = static_cast<int>((bits >> 23) & 0xffu);
    const auto mantissa = bits & 0x7fffffu;
    if (biased_exponent == 0) return sign;
    const auto exponent = biased_exponent - 127;
    if (exponent < -25) return sign;
    if (exponent < -14) {
        const auto significand = 0x800000u | mantissa;
        const auto rounded = round_shift_even(significand, static_cast<unsigned>(-exponent - 1));
        return static_cast<std::uint16_t>(sign | rounded);
    }
    if (exponent > 15) throw std::runtime_error("event strength exceeds durable f16 capacity");
    auto rounded = round_shift_even(mantissa, 13);
    auto stored_exponent = exponent + 15;
    if (rounded == 1024) {
        rounded = 0;
        ++stored_exponent;
    }
    if (stored_exponent >= 31)
        throw std::runtime_error("event strength exceeds durable f16 capacity");
    return static_cast<std::uint16_t>(sign | (stored_exponent << 10) | rounded);
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_signal.py@7536139:43-49
float half_value(std::uint16_t bits) {
    const auto magnitude = bits & 0x7fffu;
    const auto exponent = (magnitude >> 10) & 0x1fu;
    const auto mantissa = magnitude & 0x3ffu;
    float result = 0;
    if (exponent == 0) result = std::ldexp(static_cast<float>(mantissa), -24);
    else result = std::ldexp(static_cast<float>(1024 + mantissa), exponent - 25);
    return (bits & 0x8000u) ? -result : result;
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_signal.py@7536139:38-50
float stored_strength(double value, std::string_view dtype) {
    if (dtype != "<f4" && dtype != "<f2")
        throw std::runtime_error("unsupported event strength storage dtype");
    if (!std::isfinite(value) ||
        std::fabs(value) > static_cast<double>(std::numeric_limits<float>::max()))
        throw std::runtime_error("event arithmetic produced an unrepresentable value");
    float stored = static_cast<float>(value);
    if (dtype == "<f2") {
        stored = half_value(half_bits(stored));
        if ((value >= 1.0) != (stored >= 1.0f))
            throw std::runtime_error("durable f16 rounding changes experience promotion");
    }
    return stored;
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_signal.py@7536139:67-71
std::uint32_t edge_address(std::string_view text, std::uint64_t edge_count) {
    if (text.empty()) throw std::runtime_error("invalid connection address");
    for (const unsigned char character : text)
        if (character < '0' || character > '9')
            throw std::runtime_error("invalid connection address");
    std::uint64_t parsed = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
        parsed >= edge_count)
        throw std::runtime_error("duplicate or unknown connection address");
    return static_cast<std::uint32_t>(parsed);
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_signal.py@7536139:88-92
bool same_promotion(const VRSExperiencePromotionDecision& left,
                    const VRSExperiencePromotionDecision& right) {
    return left.snapshot_id == right.snapshot_id &&
           left.connection_id == right.connection_id &&
           left.previous_strength == right.previous_strength &&
           left.current_strength == right.current_strength &&
           left.action == right.action && left.promoted == right.promoted &&
           left.semantic_evidence_allowed == right.semantic_evidence_allowed &&
           left.underlying_experience_preserved == right.underlying_experience_preserved &&
           left.action_authorized == right.action_authorized &&
           left.persistent_write_authorized == right.persistent_write_authorized;
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_kernel.py@7536139:129-131
bool same_float_bits(float left, float right) {
    return std::bit_cast<std::uint32_t>(left) == std::bit_cast<std::uint32_t>(right);
}

}  // namespace

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_event_signal.py@7536139:53-97
BoundEventStrengthUpdates bind_event_strength_updates(
    const ValidatedEventVrsInputs& inputs, const VRSStateUpdateReceipt* receipt,
    std::string_view connection_namespace, std::string_view strength_storage_dtype) {
    const auto& source = inputs.require_validated_immutable();
    BoundEventStrengthUpdates bound;
    if (!receipt) return bound;
    if (receipt->snapshot_id != source.snapshot_id())
        throw std::runtime_error("re-evidence proposal generation changed");
    if (connection_namespace != "vrs-edge:" && connection_namespace != "vrs-edge-group:")
        throw std::runtime_error("explicit connection namespace required");
    std::set<std::uint32_t> seen;
    for (const auto& row : receipt->updates) {
        if (!std::string_view(row.connection_id).starts_with(connection_namespace))
            throw std::runtime_error("re-evidence connection namespace changed");
        const auto edge = edge_address(
            std::string_view(row.connection_id).substr(connection_namespace.size()),
            source.edge_count());
        if (!seen.insert(edge).second)
            throw std::runtime_error("duplicate or unknown connection address");
        if (row.previous_strength != static_cast<double>(source.strength(edge)))
            throw std::runtime_error("re-evidence strength does not match immutable input");
        double expected = row.previous_strength;
        if (row.update_action == "reinforce" && row.verdict == "support")
            expected *= 1.01;
        else if (row.update_action == "weaken" && row.verdict == "refute")
            expected *= 0.995;
        else if (row.update_action == "abstain_conflict") {}
        else if (row.update_action == "preserve_unresolved" &&
                 row.verdict != "support" && row.verdict != "refute") {}
        else throw std::runtime_error("re-evidence proposal action changed");
        if (row.current_strength != expected)
            throw std::runtime_error("re-evidence strength operation changed");
        if (!same_promotion(row.promotion, assess_vrs_experience_promotion(
                receipt->snapshot_id, row.connection_id, row.previous_strength, expected)))
            throw std::runtime_error("re-evidence promotion binding changed");
        const auto value = stored_strength(expected, strength_storage_dtype);
        if (!same_float_bits(value, source.strength(edge))) {
            bound.strengths[edge] = value;
            bound.seed_nodes.insert(source.edge(edge).target);
        }
    }
    return bound;
}

}  // namespace swegca::vrs
