#pragma once

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace swegca::world {

struct SessionEncodedEvent;

inline constexpr std::string_view semantic_family_directory_source_sha256 =
    "796fec3bbf814e362477a4eacc12a201aad05816583160beef217e8a95cfdb6b";

struct SemanticFamilyDirectory final {
    std::map<std::string, std::vector<std::string>, std::less<>> by_parent;
    std::map<std::string, std::vector<std::string>, std::less<>> by_child;

    [[nodiscard]] static SemanticFamilyDirectory from_events(
        const std::vector<SessionEncodedEvent>& events);
    [[nodiscard]] SemanticFamilyDirectory merge(
        const SemanticFamilyDirectory& other) const;
};

[[nodiscard]] std::vector<std::string> family_keys(
    const std::vector<SemanticFamilyDirectory>& directories,
    std::string_view identifier);
[[nodiscard]] std::vector<std::vector<std::string>> family_spans(
    const std::vector<SemanticFamilyDirectory>& directories,
    std::string_view parent);

}  // namespace swegca::world
