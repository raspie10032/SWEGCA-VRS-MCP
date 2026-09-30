#pragma once

#include "world/experience_atoms.hpp"
#include "world/memory_activation.hpp"

#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view episode_atoms_source_sha256 =
    "80b4ba17cc7413ffa5124dce2302164ab4316701bb0b1d996a7d2c307795ed37";

class AtomPreparationRequired final : public std::out_of_range {
public: using std::out_of_range::out_of_range;
};

class EpisodeAtoms final {
public:
    EpisodeAtoms(const MemoryEpisode* episode, std::vector<AtomizedExperience> steps,
                 std::map<std::string, std::pair<std::size_t, std::size_t>, std::less<>> locations);
    [[nodiscard]] static EpisodeAtoms build(const MemoryEpisode& episode,
        std::size_t maximum_bytes = 4096,
        LosslessBlockCodec codec = LosslessBlockCodec::zlib);
    void verify_cold() const;
    [[nodiscard]] std::pair<std::size_t, PreparedAtom> prepare(std::string_view atom_id) const;

    const MemoryEpisode* const episode;
    const std::vector<AtomizedExperience> steps;
    const std::map<std::string, std::pair<std::size_t, std::size_t>, std::less<>> locations;
};

struct PreparedEpisodeAtom final {
    std::string memory_snapshot_id;
    const MemoryEpisode* episode{};
    std::size_t step_index{};
    PreparedAtom atom;
    [[nodiscard]] const MemoryStep& replay_step() const;
};

class AtomRecallIndex final : public HotMemoryIndex {
public:
    AtomRecallIndex(std::shared_ptr<const HotMemoryIndex> base,
                    std::map<std::string, std::shared_ptr<const EpisodeAtoms>, std::less<>> bindings);
    [[nodiscard]] std::string_view snapshot_id() const noexcept override;
    [[nodiscard]] std::size_t episode_count() const noexcept override;
    [[nodiscard]] const std::map<std::string, std::size_t, std::less<>>& outcome_counts() const noexcept override;
    [[nodiscard]] const MemoryEpisode& episode(std::string_view id) const override;
    [[nodiscard]] std::vector<std::string> episode_ids_for_cue(std::string_view cue) const override;
    [[nodiscard]] std::vector<std::string> iter_episode_ids() const override;
    [[nodiscard]] std::vector<std::string> atom_ids(std::string_view episode_id) const;
    [[nodiscard]] const MemoryEpisode& parent_for_atom(std::string_view atom_id) const;
    [[nodiscard]] PreparedEpisodeAtom prepare_atom(std::string_view atom_id) const;

    const std::shared_ptr<const HotMemoryIndex> base;
    const std::map<std::string, std::shared_ptr<const EpisodeAtoms>, std::less<>> bindings;
private:
    std::map<std::string, std::string, std::less<>> atom_parents_;
};

}  // namespace swegca::world
