#pragma once

#include "world/cognitive_state.hpp"

#include <cstdint>
#include <string_view>

namespace swegca::world {

inline constexpr std::string_view image_tag_experience_source_sha256 =
    "068534e0163b5f4a17406999d9a0bef707e091a378328f3b41f00f45d9bb41ab";
inline constexpr std::string_view media_observation_source_sha256 =
    "0e40c6706c567255945c0ff32a8a7b7ca04662a0feb31337541d0e64688c380a";
inline constexpr std::string_view authored_media_observation_schema =
    "rozephine-authored-media-observation-v1";

// Exact C++ port of the pinned image/tag row boundary. The decoded visual
// vectors and tag proposals remain one source-bound pending experience. This
// function does not verify tag semantics, claim growth, or write World state.
[[nodiscard]] JsonValue image_tag_experience(
    JsonValue::Object content, std::string_view source_family,
    std::int64_t observed_at_unix_ns);

// Validate the image_tag_observation subset used by the pinned constructor.
// This is deliberately not a claim of parity with every media kind accepted
// by mosaic_media_observation.py.
void validate_image_tag_experience(const JsonValue& episode);

}  // namespace swegca::world
