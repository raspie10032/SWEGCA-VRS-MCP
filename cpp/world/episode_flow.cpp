#include "world/episode_flow.hpp"

#include "world/semantic_vrs_ingress.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <utility>

namespace swegca::world {
namespace {

JsonValue strings(const std::vector<std::string>& values) {
    JsonValue::Array result;
    result.reserve(values.size());
    for (const auto& value : values) result.emplace_back(value);
    return result;
}

std::vector<std::string> unique_refs(std::vector<std::string> refs) {
    std::set<std::string, std::less<>> seen;
    std::vector<std::string> result;
    result.reserve(refs.size());
    for (auto& ref : refs) if (seen.insert(ref).second) result.push_back(std::move(ref));
    return result;
}

std::vector<std::string> refs_for(const MemoryEpisode& episode, const MemoryStep& step) {
    auto refs = episode.source_addresses;
    refs.insert(refs.end(), step.evidence_refs.begin(), step.evidence_refs.end());
    return refs;
}

JsonValue path_json(const std::vector<ObservationPathElement>& path) {
    JsonValue::Array result;
    for (const auto& part : path) {
        if (const auto* text = std::get_if<std::string>(&part)) result.emplace_back(*text);
        else result.emplace_back(static_cast<std::int64_t>(std::get<std::size_t>(part)));
    }
    return result;
}

std::string node_id(const EpisodeAtoms& binding, const std::size_t index,
                    const std::string& field,
                    const std::vector<ObservationPathElement>& path,
                    const std::string& kind,
                    const std::optional<std::string>& media_address) {
    return experience_atom_identifier(JsonValue::Array{
        "replay-flow-node-v1", binding.episode->episode_id, binding.episode->revision,
        std::string(binding.steps[index].source_hash()), static_cast<std::int64_t>(index),
        field, path_json(path), kind,
        media_address ? JsonValue(*media_address) : JsonValue(nullptr)});
}

JsonValue step_field(const MemoryStep& step, const std::string_view field) {
    if (field == "observation") return step.observation;
    if (field == "phase") return step.phase;
    if (field == "relations") return strings(step.relations);
    if (field == "judgment") return step.judgment;
    if (field == "outcome") return step.outcome;
    if (field == "evidence_refs") return strings(step.evidence_refs);
    throw std::out_of_range("unknown replay flow field");
}

const JsonValue* member(const JsonValue::Object& value, const std::string_view key) {
    const auto found = value.find(key);
    return found == value.end() ? nullptr : &found->second;
}

std::optional<std::string> optional_text(const JsonValue::Object& value,
                                         const std::string_view key) {
    const auto* item = member(value, key);
    if (!item) return std::nullopt;
    if (const auto* text = std::get_if<std::string>(&item->storage())) return *text;
    return std::nullopt;
}

std::optional<std::int64_t> optional_integer(const JsonValue::Object& value,
                                             const std::string_view key) {
    const auto* item = member(value, key);
    if (!item) return std::nullopt;
    if (const auto* number = std::get_if<std::int64_t>(&item->storage())) return *number;
    return std::nullopt;
}

bool truthy(const JsonValue* value) {
    if (!value || std::holds_alternative<std::nullptr_t>(value->storage())) return false;
    if (const auto* item = std::get_if<bool>(&value->storage())) return *item;
    if (const auto* item = std::get_if<std::int64_t>(&value->storage())) return *item != 0;
    if (const auto* item = std::get_if<JsonInteger>(&value->storage())) return item->value != "0";
    if (const auto* item = std::get_if<double>(&value->storage())) return *item != 0.0;
    if (const auto* item = std::get_if<std::string>(&value->storage())) return !item->empty();
    if (const auto* item = std::get_if<JsonValue::Array>(&value->storage())) return !item->empty();
    return !std::get<JsonValue::Object>(value->storage()).empty();
}

}  // namespace

struct EpisodeFlow::BuildResult final {
    std::map<std::string, FlowAddress, std::less<>> nodes;
    std::vector<FlowLink> links;
    std::map<std::string, std::vector<FlowLink>, std::less<>> incoming;
    std::map<std::string, std::vector<FlowLink>, std::less<>> outgoing;
};

const MemoryStep& PreparedFlow::replay_step() const {
    return episode->steps.at(node.step_index);
}

EpisodeFlow::EpisodeFlow(EpisodeMediaSelectors media_value)
    : EpisodeFlow(media_value, build(media_value)) {}

EpisodeFlow::EpisodeFlow(EpisodeMediaSelectors media_value, BuildResult built)
    : media(std::move(media_value)), nodes(std::move(built.nodes)),
      links(std::move(built.links)), incoming(std::move(built.incoming)),
      outgoing(std::move(built.outgoing)) {}

EpisodeFlow::BuildResult EpisodeFlow::build(const EpisodeMediaSelectors& media) {
    const auto verified = EpisodeMediaSelectors::build(*media.binding);
    if (verified.selectors != media.selectors)
        throw std::invalid_argument("media selector generation differs from source");
    const auto& binding = *media.binding;
    const auto& episode = *binding.episode;
    BuildResult result;

    const auto add = [&](const std::size_t index, std::string field,
                         std::vector<ObservationPathElement> path, std::string kind,
                         std::optional<std::string> media_address = std::nullopt) {
        const auto address = node_id(binding, index, field, path, kind, media_address);
        FlowAddress node{address, index, std::move(field), std::move(path),
                         std::move(kind), std::move(media_address)};
        if (!result.nodes.emplace(address, node).second)
            throw std::invalid_argument("duplicate flow address");
        return node;
    };
    const auto link = [&](const FlowAddress& source, const FlowAddress& target,
                          std::string kind, std::vector<std::string> refs) {
        refs = unique_refs(std::move(refs));
        JsonValue::Array refs_json;
        for (const auto& ref : refs) refs_json.emplace_back(ref);
        const auto identity = experience_atom_identifier(JsonValue::Array{
            "replay-flow-link-v1", source.address, target.address, kind, refs_json});
        result.links.push_back({identity, source.address, target.address,
                                std::move(kind), std::move(refs)});
    };

    std::vector<FlowAddress> roots;
    roots.reserve(episode.steps.size());
    for (std::size_t index = 0; index < episode.steps.size(); ++index) {
        const auto& step = episode.steps[index];
        auto refs = refs_for(episode, step);
        auto root = add(index, "observation", {}, "step_observation");
        roots.push_back(root);
        std::map<std::string, FlowAddress, std::less<>> fields;
        for (const auto* field : {"phase", "relations", "judgment", "outcome", "evidence_refs"}) {
            auto node = add(index, field, {}, std::string("recorded_") + field);
            link(root, node, "step_contains", refs);
            fields.emplace(field, std::move(node));
        }
        if (index != 0) {
            auto order_refs = refs;
            const auto& prior = episode.steps[index - 1].evidence_refs;
            order_refs.insert(order_refs.end(), prior.begin(), prior.end());
            link(roots[index - 1], root, "recorded_step_order", std::move(order_refs));
        }
        if (step.phase == "observation_attempt_outcome") {
            const auto action = optional_text(step.observation, "action");
            const auto candidate = member(step.observation, "candidate_id");
            const auto task = member(step.observation, "task_id");
            const auto relation = action ? "physical-action:" + *action + "->outcome:" + step.outcome : "";
            if (action && !action->empty() &&
                std::ranges::find(step.relations, relation) != step.relations.end() &&
                truthy(candidate) && truthy(task)) {
                auto attempt = add(index, "observation", {std::string("action")},
                                   "recorded_action_attempt");
                link(root, attempt, "step_contains", refs);
                link(attempt, fields.at("outcome"), "recorded_attempt_outcome", refs);
            }
        }
    }

    std::optional<std::pair<FlowAddress, std::size_t>> prior_frame;
    struct PriorAudio { FlowAddress node; std::string source; std::int64_t sequence{}; std::size_t step_index{}; };
    std::optional<PriorAudio> prior_audio;
    for (std::size_t selector_index = 0; selector_index < media.selectors.size(); ++selector_index) {
        const auto& selector = media.selectors[selector_index];
        if (selector.kind == "retained_observation") continue;
        const auto index = selector.step_index;
        const auto& step = episode.steps[index];
        auto refs = refs_for(episode, step);
        auto node = add(index, "observation", selector.observation_path,
                        selector.kind, selector.atom_id);
        link(roots[index], node, "step_contains", refs);
        if (selector.kind == "screen_frame") {
            if (prior_frame) {
                const auto& old = media.selectors[prior_frame->second];
                if (old.step_index + 1 == index && old.temporal && selector.temporal &&
                    old.temporal->clock == selector.temporal->clock &&
                    old.temporal->stop_ns < selector.temporal->start_ns) {
                    auto pair_refs = refs;
                    const auto& old_refs = episode.steps[old.step_index].evidence_refs;
                    pair_refs.insert(pair_refs.end(), old_refs.begin(), old_refs.end());
                    link(prior_frame->first, node, "recorded_frame_pair", std::move(pair_refs));
                }
            }
            prior_frame = std::pair{node, selector_index};
        }
        if (selector.kind == "system_audio_feature_segment" ||
            selector.kind == "audio_feature_record") {
            const auto root = JsonValue(step.observation);
            const auto& value = observation_at(root, selector.observation_path);
            if (!value.is_object()) throw std::invalid_argument("recorded acoustic value changed");
            const auto& object = value.as_object();
            if (const auto* silence_value = member(object, "digital_silence")) {
                const auto* silence = std::get_if<bool>(&silence_value->storage());
                if (!silence)
                    throw std::invalid_argument("recorded digital silence must be boolean or absent");
                auto path = selector.observation_path;
                path.emplace_back(std::string("digital_silence"));
                auto flag = add(index, "observation", std::move(path),
                    *silence ? "recorded_digital_silence" : "recorded_nonzero_pcm",
                    selector.atom_id);
                link(node, flag, "recorded_acoustic_property", refs);
            }
            if (selector.kind == "system_audio_feature_segment") {
                const auto source = optional_text(object, "source");
                const auto sequence = optional_integer(object, "sequence");
                if (source && sequence) {
                    if (prior_audio && *source == prior_audio->source &&
                        *sequence == prior_audio->sequence + 1) {
                        auto sequence_refs = refs;
                        const auto& old_refs = episode.steps[prior_audio->step_index].evidence_refs;
                        sequence_refs.insert(sequence_refs.end(), old_refs.begin(), old_refs.end());
                        link(prior_audio->node, node, "recorded_audio_sequence",
                             std::move(sequence_refs));
                    }
                    prior_audio = PriorAudio{node, *source, *sequence, index};
                } else {
                    prior_audio.reset();
                }
            }
        }
    }
    for (const auto& [address, unused] : result.nodes) {
        static_cast<void>(unused);
        result.incoming.emplace(address, std::vector<FlowLink>{});
        result.outgoing.emplace(address, std::vector<FlowLink>{});
    }
    for (const auto& edge : result.links) {
        result.outgoing.at(edge.source).push_back(edge);
        result.incoming.at(edge.target).push_back(edge);
    }
    return result;
}

PreparedFlow EpisodeFlow::prepare(const std::string_view address) const {
    const auto& node = nodes.at(address);
    const auto& binding = *media.binding;
    const auto& step = binding.episode->steps.at(node.step_index);
    auto value = step_field(step, node.field);
    const auto& selected = observation_at(value, node.path);
    const auto wire = semantic_canonical_json(selected);
    return {node, binding.episode, std::string(binding.steps[node.step_index].source_hash()),
            {reinterpret_cast<const std::byte*>(wire.data()),
             reinterpret_cast<const std::byte*>(wire.data() + wire.size())},
            incoming.at(node.address), outgoing.at(node.address)};
}

}  // namespace swegca::world
