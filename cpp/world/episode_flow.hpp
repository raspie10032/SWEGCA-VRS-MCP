#pragma once

#include "world/media_atoms.hpp"

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view episode_flow_source_sha256 =
    "3786af330579fb877c8db2ac869b98143ff599bf440caf2080e99beb5438b8a0";

struct FlowAddress final {
    std::string address;
    std::size_t step_index{};
    std::string field;
    std::vector<ObservationPathElement> path;
    std::string kind;
    std::optional<std::string> media_address;
    friend bool operator==(const FlowAddress&, const FlowAddress&) = default;
};

struct FlowLink final {
    std::string address;
    std::string source;
    std::string target;
    std::string kind;
    std::vector<std::string> evidence_refs;
    friend bool operator==(const FlowLink&, const FlowLink&) = default;
};

struct PreparedFlow final {
    FlowAddress node;
    const MemoryEpisode* episode{};
    std::string parent_step_hash;
    std::vector<std::byte> payload;
    std::vector<FlowLink> incoming;
    std::vector<FlowLink> outgoing;
    [[nodiscard]] const MemoryStep& replay_step() const;
};

class EpisodeFlow final {
public:
    explicit EpisodeFlow(EpisodeMediaSelectors media);
    [[nodiscard]] PreparedFlow prepare(std::string_view address) const;

    const EpisodeMediaSelectors media;
    const std::map<std::string, FlowAddress, std::less<>> nodes;
    const std::vector<FlowLink> links;
    const std::map<std::string, std::vector<FlowLink>, std::less<>> incoming;
    const std::map<std::string, std::vector<FlowLink>, std::less<>> outgoing;

private:
    struct BuildResult;
    EpisodeFlow(EpisodeMediaSelectors media, BuildResult built);
    [[nodiscard]] static BuildResult build(const EpisodeMediaSelectors& media);
};

}  // namespace swegca::world
