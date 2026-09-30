#include "world/sealed_generation_restore.hpp"

#include "world/atom_hot_index.hpp"
#include "world/compressed_memory.hpp"

#include <limits>
#include <set>
#include <stdexcept>

namespace swegca::world {
namespace {

FullCurrentMemoryVrsSnapshot bind_ordinary(
    std::vector<std::shared_ptr<const HotMemoryIndex>> ordinary,
    std::shared_ptr<const VrsHotMemorySource> replacement,
    std::string report_sha256, std::vector<std::string> replaced,
    const std::string_view expected_memory, const std::string_view expected_pair) {
    auto memory = std::make_shared<const VrsGenerationBoundMemoryIndex>(
        std::move(ordinary), std::move(replacement), report_sha256, std::move(replaced));
    FullCurrentMemoryVrsSnapshot pair(memory, std::move(report_sha256));
    if (memory->snapshot_id() != expected_memory || pair.snapshot_id != expected_pair)
        throw std::invalid_argument("sealed final-generation identity differs");
    return pair;
}

std::vector<std::shared_ptr<const HotMemoryIndex>> restore_ordinary(
    const FullCurrentMemoryVrsSnapshot& base,
    const std::vector<std::vector<MemoryEpisode>>& waves) {
    if (const auto atoms = std::dynamic_pointer_cast<const AtomHotMemoryIndex>(base.memory)) {
        std::shared_ptr<const AtomHotMemoryIndex> staged = atoms;
        const std::vector<std::string> required(memory_outcomes.begin(), memory_outcomes.end());
        for (const auto& wave : waves) staged = staged->append(wave, required);
        std::vector<std::shared_ptr<const HotMemoryIndex>> result;
        for (const auto& leaf : vrs_leaf_sources(staged))
            if (!std::dynamic_pointer_cast<const VrsHotMemorySource>(leaf)) result.push_back(leaf);
        return result;
    }
    std::vector<std::shared_ptr<const HotMemoryIndex>> ordinary;
    for (const auto& leaf : vrs_leaf_sources(base.memory))
        if (!std::dynamic_pointer_cast<const VrsHotMemorySource>(leaf)) ordinary.push_back(leaf);
    const auto policy = inherited_compression_policy(base.memory);
    auto counts = base.memory->outcome_counts();
    std::set<std::string, std::less<>> appended;
    for (const auto& wave : waves) {
        for (const auto& episode : wave)
            if (!appended.insert(episode.episode_id).second ||
                base.memory->contains_episode(episode.episode_id))
                throw std::invalid_argument("memory episode IDs must be unique");
        if (!wave.empty()) {
            std::shared_ptr<const HotMemoryIndex> leaf = build_memory_activation_index(wave);
            if (policy) leaf = compress_hot_memory_index(std::move(leaf), *policy);
            ordinary.push_back(leaf);
            for (const auto& outcome : memory_outcomes)
                counts[outcome] += leaf->outcome_counts().at(outcome);
        }
        for (const auto& outcome : memory_outcomes)
            if (!counts.at(outcome))
                throw std::invalid_argument("missing required historical outcomes");
    }
    return ordinary;
}

}  // namespace

std::vector<std::shared_ptr<const HotMemoryIndex>> restore_resident_ordinary_leaves(
    const std::vector<std::span<const std::byte>>& archives,
    const std::vector<ResidentLeafSeal>& seals,
    const std::size_t maximum_archive_bytes) {
    if (archives.size() != seals.size())
        throw std::invalid_argument("resident generation archive count differs");
    std::size_t total{};
    for (const auto& seal : seals) {
        if (seal.bytes > maximum_archive_bytes - total)
            throw std::invalid_argument("resident generation exceeds cold archive budget");
        total += seal.bytes;
    }
    std::vector<std::shared_ptr<const HotMemoryIndex>> ordinary;
    std::set<std::string, std::less<>> addresses;
    for (std::size_t i = 0; i < archives.size(); ++i) {
        if (archives[i].size() != seals[i].bytes)
            throw std::invalid_argument("resident generation leaf extent differs");
        auto leaf = load_directory_leaf(archives[i], seals[i].sha256,
                                        seals[i].snapshot_id, seals[i].bytes);
        for (const auto& address : leaf->iter_episode_ids()) {
            bool virtual_address{};
            for (const auto prefix : vrs_virtual_episode_prefixes)
                virtual_address = virtual_address || address.starts_with(prefix);
            if (virtual_address || !addresses.insert(address).second)
                throw std::invalid_argument("resident ordinary address overlap");
        }
        ordinary.push_back(std::move(leaf));
    }
    return ordinary;
}

FullCurrentMemoryVrsSnapshot restore_sealed_final_generation(
    const FullCurrentMemoryVrsSnapshot& base,
    const std::vector<std::vector<MemoryEpisode>>& waves,
    std::shared_ptr<const VrsHotMemorySource> replacement,
    std::string vrs_report_sha256,
    std::vector<std::string> replaced_vrs_source_snapshot_ids,
    const std::string_view expected_memory_snapshot_id,
    const std::string_view expected_pair_snapshot_id) {
    return bind_ordinary(restore_ordinary(base, waves), std::move(replacement),
        std::move(vrs_report_sha256), std::move(replaced_vrs_source_snapshot_ids),
        expected_memory_snapshot_id, expected_pair_snapshot_id);
}

FullCurrentMemoryVrsSnapshot restore_resident_final_generation(
    const std::vector<std::span<const std::byte>>& archives,
    const std::vector<ResidentLeafSeal>& seals,
    const std::size_t maximum_archive_bytes,
    std::shared_ptr<const VrsHotMemorySource> replacement,
    std::string vrs_report_sha256,
    std::vector<std::string> replaced_vrs_source_snapshot_ids,
    const std::string_view expected_memory_snapshot_id,
    const std::string_view expected_pair_snapshot_id) {
    return bind_ordinary(restore_resident_ordinary_leaves(
            archives, seals, maximum_archive_bytes), std::move(replacement),
        std::move(vrs_report_sha256), std::move(replaced_vrs_source_snapshot_ids),
        expected_memory_snapshot_id, expected_pair_snapshot_id);
}

}  // namespace swegca::world
