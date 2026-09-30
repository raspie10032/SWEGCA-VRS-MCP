#pragma once

#include "world/episode_atoms.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view media_atoms_source_sha256 =
    "54cd1e8273f731d3475075d547ff472a77eae750dfbb8107e55ceac9bdd763b6";

using ObservationPathElement = std::variant<std::string, std::size_t>;

struct SpatialExtent final {
    std::pair<std::int64_t, std::int64_t> native_wh;
    std::array<std::int64_t, 4> original_xywh;
    std::string coordinate_frame{"native_source_pixels"};
    SpatialExtent(std::pair<std::int64_t, std::int64_t> native_wh,
                  std::array<std::int64_t, 4> original_xywh,
                  std::string coordinate_frame = "native_source_pixels");
    friend bool operator==(const SpatialExtent&, const SpatialExtent&) = default;
};

struct TemporalExtent final {
    std::string clock;
    std::int64_t start_ns{};
    std::int64_t stop_ns{};
    std::string precision;
    std::optional<std::int64_t> hardware_latency_ns;
    TemporalExtent(std::string clock, std::int64_t start_ns, std::int64_t stop_ns,
                   std::string precision,
                   std::optional<std::int64_t> hardware_latency_ns = std::nullopt);
    friend bool operator==(const TemporalExtent&, const TemporalExtent&) = default;
};

struct SampleExtent final {
    std::int64_t start_frame{};
    std::int64_t stop_frame{};
    std::int64_t sample_rate{};
    SampleExtent(std::int64_t start_frame, std::int64_t stop_frame,
                 std::int64_t sample_rate);
    [[nodiscard]] std::pair<std::int64_t, std::int64_t> enclosing_ns() const;
    friend bool operator==(const SampleExtent&, const SampleExtent&) = default;
};

struct MediaSelector final {
    std::string atom_id;
    std::size_t step_index{};
    std::string kind;
    std::vector<ObservationPathElement> observation_path;
    std::optional<SpatialExtent> spatial;
    std::optional<TemporalExtent> temporal;
    std::optional<SampleExtent> samples;
    friend bool operator==(const MediaSelector&, const MediaSelector&) = default;
};

struct PreparedMediaSelector final {
    MediaSelector selector;
    const MemoryEpisode* episode{};
    std::string parent_step_hash;
    std::vector<std::byte> payload;
    [[nodiscard]] const MemoryStep& replay_step() const;
};

class EpisodeMediaSelectors final {
public:
    EpisodeMediaSelectors(const EpisodeAtoms* binding,
                          std::vector<MediaSelector> selectors);
    [[nodiscard]] static EpisodeMediaSelectors build(const EpisodeAtoms& binding);
    [[nodiscard]] PreparedMediaSelector prepare(std::size_t index) const;
    const EpisodeAtoms* const binding;
    const std::vector<MediaSelector> selectors;
};

[[nodiscard]] const JsonValue& observation_at(
    const JsonValue& value, const std::vector<ObservationPathElement>& path);

}  // namespace swegca::world
