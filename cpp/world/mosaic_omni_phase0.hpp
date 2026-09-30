#pragma once

#include "world/modal_to_world.hpp"
#include "world/mosaic_omni.hpp"
#include "world/mosaic_resource_profile.hpp"
#include "world/mosaic_te.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace swegca::world {

inline constexpr std::string_view mosaic_omni_phase0_source_sha256 =
    "a06f7a85128827b036a7c1801920e570e187c0163df6c3851d85ef2dd3bb5e20";

struct MosaicOmniPhase0Dependencies final {
    MosaicTextEncoderProbe& text_encoder;
    const ModalToWorldAdapter& gemma_adapter;
    const WorldToAnimaConditioning& anima_adapter;
    // ModalToWorldAdapter and WorldToAnimaConditioning keep weights private;
    // their exact parameter total is supplied by the constructing owner.
    std::uint64_t injected_adapter_parameter_count{};
};

[[nodiscard]] JsonValue::Object run_mosaic_omni_phase0_probe(
    const MosaicOmniConfig& config, std::size_t repeats,
    const JsonValue::Object& local_contract,
    const MosaicOmniPhase0Dependencies& dependencies);

}  // namespace swegca::world
