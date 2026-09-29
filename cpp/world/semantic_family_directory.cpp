#include "world/semantic_family_directory.hpp"

#include "world/session_content_encoding.hpp"

#include <algorithm>

namespace swegca::world {
namespace {

void append_unique(std::vector<std::string>& rows, const std::string& value) {
    if (std::ranges::find(rows, value) == rows.end()) rows.push_back(value);
}

}  // namespace

SemanticFamilyDirectory SemanticFamilyDirectory::from_events(
    const std::vector<SessionEncodedEvent>& events) {
    SemanticFamilyDirectory result;
    for (const auto& event : events) {
        if (!event.semantic_encoding ||
            std::ranges::find(event.unresolved, "semantic_source_binding_unresolved") !=
                event.unresolved.end()) continue;
        const auto parents = event.semantic_encoding->source_episode_ids();
        for (const auto& parent : parents) {
            append_unique(result.by_parent[parent], event.episode_id);
            append_unique(result.by_child[event.episode_id], parent);
        }
    }
    return result;
}

SemanticFamilyDirectory SemanticFamilyDirectory::merge(
    const SemanticFamilyDirectory& other) const {
    auto result = *this;
    for (const auto& [key, values] : other.by_parent)
        for (const auto& value : values) append_unique(result.by_parent[key], value);
    for (const auto& [key, values] : other.by_child)
        for (const auto& value : values) append_unique(result.by_child[key], value);
    return result;
}

std::vector<std::string> family_keys(
    const std::vector<SemanticFamilyDirectory>& directories,
    const std::string_view identifier) {
    std::vector<std::string> result;
    const std::string key(identifier);
    for (const auto& directory : directories) {
        if (directory.by_parent.contains(key)) append_unique(result, key);
        if (const auto found = directory.by_child.find(key); found != directory.by_child.end())
            for (const auto& parent : found->second) append_unique(result, parent);
    }
    return result;
}

std::vector<std::vector<std::string>> family_spans(
    const std::vector<SemanticFamilyDirectory>& directories,
    const std::string_view parent) {
    std::vector<std::vector<std::string>> result{{std::string(parent)}};
    for (const auto& directory : directories) {
        const auto found = directory.by_parent.find(parent);
        if (found != directory.by_parent.end() && !found->second.empty())
            result.push_back(found->second);
    }
    return result;
}

}  // namespace swegca::world
