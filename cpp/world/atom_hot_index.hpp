#pragma once

#include "world/episode_flow.hpp"
#include "world/vrs_generation_rebind.hpp"

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view atom_hot_index_source_sha256 =
    "bb5b3aa7845b4e31c46295ab1f2fa0304dc16ab22f97dca8f4aa6b0ed42cf8d8";

struct AtomHotSidecar final {
    std::shared_ptr<const EpisodeAtoms> binding;
    std::shared_ptr<const EpisodeMediaSelectors> media;
    std::shared_ptr<const EpisodeFlow> flow;
};

using PreparedHotItem = std::variant<PreparedEpisodeAtom, PreparedMediaSelector, PreparedFlow>;

struct PreparedHotAddress final {
    std::string memory_snapshot_id;
    std::string parent_episode_id;
    std::string kind;
    PreparedHotItem item;
};

class AtomHotMemoryIndex final : public HotMemoryIndex,
                                 public std::enable_shared_from_this<AtomHotMemoryIndex> {
public:
    struct Segment;

    [[nodiscard]] static std::shared_ptr<const AtomHotMemoryIndex> build(
        std::shared_ptr<const HotMemoryIndex> base,
        const std::vector<std::string>& prepared_episode_ids = {},
        std::size_t maximum_bytes = 4096);

    [[nodiscard]] std::string_view snapshot_id() const noexcept override;
    [[nodiscard]] std::size_t episode_count() const noexcept override;
    [[nodiscard]] const std::map<std::string, std::size_t, std::less<>>&
        outcome_counts() const noexcept override;
    [[nodiscard]] const MemoryEpisode& episode(std::string_view episode_id) const override;
    [[nodiscard]] std::vector<std::string> episode_ids_for_cue(
        std::string_view cue) const override;
    [[nodiscard]] std::vector<std::string> iter_episode_ids() const override;

    [[nodiscard]] const AtomHotSidecar& sidecar(std::string_view episode_id) const;
    [[nodiscard]] std::vector<std::string> addresses(std::string_view episode_id) const;
    [[nodiscard]] PreparedHotAddress prepare_address(std::string_view address) const;
    [[nodiscard]] std::shared_ptr<const AtomHotMemoryIndex> append(
        const std::vector<MemoryEpisode>& episodes,
        const std::vector<std::string>& required_outcomes = {}) const;
    [[nodiscard]] std::pair<std::shared_ptr<const AtomHotMemoryIndex>, std::size_t>
        after_vrs_rebind(std::shared_ptr<const HotMemoryIndex> replacement) const;

    const std::shared_ptr<const HotMemoryIndex> base;
    const std::vector<std::shared_ptr<const Segment>> segments;
    const std::size_t maximum_bytes;

private:
    AtomHotMemoryIndex(std::shared_ptr<const HotMemoryIndex> base,
                       std::vector<std::shared_ptr<const Segment>> segments,
                       std::size_t maximum_bytes);
};

}  // namespace swegca::world
