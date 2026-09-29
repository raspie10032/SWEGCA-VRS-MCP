#include "world/semantic_input_scope.hpp"

#include "world/semantic_encoding.hpp"
#include "world/unicode_nfkc.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>
#include <unordered_set>

namespace swegca::world {
namespace {

using Path = std::vector<SemanticPathElement>;
using Paths = std::set<Path>;

const std::set<std::string, std::less<>> authored_fields{
    "schema_version", "source_id", "source_item_id", "source_revision_receipt",
    "source_family", "task_family", "observation_query", "media_kind", "content",
    "semantics_verified", "growth_claimed", "translation_variants_are_new_events",
    "source_identity", "observed_at_unix_ns", "source_type", "source_origin",
    "actual_world_outcomes_claimed", "source_observation_count",
    "synthetic_examples_count_as_new_experience"};

const JsonValue* find(const JsonValue::Object& object, const std::string_view key) {
    const auto item = object.find(key);
    return item == object.end() ? nullptr : &item->second;
}

bool string_is(const JsonValue* value, const std::string_view expected) {
    const auto* text = value ? std::get_if<std::string>(&value->storage()) : nullptr;
    return text && *text == expected;
}

bool boolean_is(const JsonValue* value, const bool expected) {
    const auto* boolean = value ? std::get_if<bool>(&value->storage()) : nullptr;
    return boolean && *boolean == expected;
}

std::int64_t integer(const JsonValue* value, const char* reason) {
    const auto* number = value ? std::get_if<std::int64_t>(&value->storage()) : nullptr;
    if (!number) throw std::invalid_argument(reason);
    return *number;
}

bool subset(const JsonValue::Object& object,
            const std::set<std::string, std::less<>>& allowed) {
    return std::ranges::all_of(object, [&](const auto& item) {
        return allowed.contains(item.first);
    });
}

std::set<std::string, std::less<>> object_keys(const JsonValue::Object& object) {
    std::set<std::string, std::less<>> result;
    for (const auto& [key, _] : object) result.insert(key);
    return result;
}

bool authored_layout_known(const JsonValue::Object& observation) {
    if (!subset(observation, authored_fields)) return false;
    const auto* content = find(observation, "content");
    if (!content || !content->is_object() || content->as_object().size() != 1 ||
        !content->as_object().contains("variants") ||
        !content->at("variants").is_array()) return false;
    static const std::set<std::string, std::less<>> fields{
        "text", "language", "source_address", "source_sha256"};
    return std::ranges::all_of(content->at("variants").as_array(), [&](const auto& value) {
        if (!value.is_object() || value.as_object().size() != fields.size()) return false;
        return std::ranges::all_of(fields, [&](const auto& key) {
            return value.as_object().contains(key);
        });
    });
}

bool digest(const JsonValue* value) {
    const auto* text = value ? std::get_if<std::string>(&value->storage()) : nullptr;
    return text && text->size() == 64 && std::ranges::all_of(*text, [](const char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

void validate_authored_observation(const SemanticMemoryStep& step,
                                   const std::vector<std::string>& cues) {
    if (!step.observation.is_object()) throw std::invalid_argument("media observation/authority boundary changed");
    const auto& observation = step.observation.as_object();
    const auto* query_value = find(observation, "observation_query");
    const auto* query = query_value ? std::get_if<std::string>(&query_value->storage()) : nullptr;
    if (!string_is(find(observation, "schema_version"), authored_media_observation_schema) ||
        step.phase != "observation_attempt_outcome" ||
        (step.outcome != "pending" && step.outcome != "uncertain" && step.outcome != "failure") ||
        !boolean_is(find(observation, "semantics_verified"), false) ||
        !boolean_is(find(observation, "growth_claimed"), false) ||
        !boolean_is(find(observation, "translation_variants_are_new_events"), false) ||
        !query || std::ranges::find(cues, *query) == cues.end() || !query->starts_with("authored-source:") ||
        std::ranges::any_of(cues, [](const auto& cue) {
            return cue.starts_with("actual-relation:") || cue.starts_with("phase7-temporal-profile-v2:");
        }) || std::ranges::any_of(step.relations, [](const auto& cue) {
            return cue.starts_with("actual-relation:") || cue.starts_with("phase7-temporal-profile-v2:");
        })) throw std::invalid_argument("media observation/authority boundary changed");
    for (const auto key : {"source_id", "source_revision_receipt", "source_item_id",
                           "source_family", "task_family"}) {
        const auto* value = find(observation, key);
        const auto* text = value ? std::get_if<std::string>(&value->storage()) : nullptr;
        if (!text || strip_unicode_whitespace(*text).empty())
            throw std::invalid_argument("media source binding incomplete");
    }
    if (!digest(find(observation, "source_revision_receipt")))
        throw std::invalid_argument("media observation revision must be content addressed");
    const auto* content_value = find(observation, "content");
    if (!content_value || !content_value->is_object() || content_value->as_object().empty())
        throw std::invalid_argument("actual observed media content required");
    const auto& content = content_value->as_object();
    const auto* source_id = std::get_if<std::string>(&find(observation, "source_id")->storage());
    const auto* source_type_value = find(observation, "source_type");
    const auto* source_type = source_type_value ? std::get_if<std::string>(&source_type_value->storage()) : nullptr;
    const auto* origin_value = find(observation, "source_origin");
    const auto* origin = origin_value ? std::get_if<std::string>(&origin_value->storage()) : nullptr;
    static const std::map<std::string, std::string, std::less<>> origins{
        {"user_statement", "conversation"}, {"public_source_paraphrase", "public_web"},
        {"public_source_text", "public_web"}, {"codex_session_record", "codex"},
        {"codex_authored_teaching", "codex"}, {"model_authored_teaching", "model_specialist"}};
    if (!string_is(find(observation, "media_kind"), "authored_text") ||
        *query != "authored-source:" + *source_id || !source_type || !origins.contains(*source_type) ||
        !origin || origins.at(*source_type) != *origin ||
        integer(find(observation, "actual_world_outcomes_claimed"), "authored explanation provenance boundary changed") != 0 ||
        integer(find(observation, "source_observation_count"), "authored explanation provenance boundary changed") != 1 ||
        !boolean_is(find(observation, "synthetic_examples_count_as_new_experience"), false) ||
        integer(find(observation, "observed_at_unix_ns"), "authored explanation provenance boundary changed") <= 0)
        throw std::invalid_argument("authored explanation provenance boundary changed");
    const auto* variants = find(content, "variants");
    if (!variants || !variants->is_array() || variants->as_array().empty())
        throw std::invalid_argument("authored text variants required");
    for (const auto& variant : variants->as_array()) {
        if (!variant.is_object() ||
            !std::holds_alternative<std::string>(variant.at("text").storage()) ||
            !find(variant.as_object(), "source_address") ||
            !find(variant.as_object(), "language") ||
            !digest(find(variant.as_object(), "source_sha256")))
            throw std::invalid_argument("text content/language/source binding incomplete");
    }
}

std::map<std::int64_t, Paths> authored_context_paths(
    const SemanticSourceEpisode& source, const SemanticEncoding& encoding,
    const SemanticContextByAnchor& bound) {
    std::map<std::int64_t, std::vector<const SemanticAnchor*>> candidates;
    for (const auto& anchor : encoding.anchors)
        if (bound.contains(anchor.identifier)) candidates[anchor.step].push_back(&anchor);
    std::map<std::int64_t, Paths> paths;
    static const std::set<std::string, std::less<>> content_fields{
        "variants", "voice_alignment", "grouping", "authored_translation_binding",
        "namespace", "dialogue_key"};
    static const std::set<std::string, std::less<>> variant_fields{
        "text", "language", "source_address", "source_sha256", "authored_record_id",
        "authored_metadata", "authored_source", "dialogue_pairing"};
    for (const auto& [index, anchors] : candidates) {
        if (index < 0 || static_cast<std::size_t>(index) >= source.steps.size()) continue;
        const auto& step = source.steps[static_cast<std::size_t>(index)];
        if (!step.observation.is_object()) continue;
        const auto& observation = step.observation.as_object();
        if (!string_is(find(observation, "schema_version"), authored_media_observation_schema) ||
            !string_is(find(observation, "media_kind"), "authored_text") ||
            !subset(observation, authored_fields) || authored_layout_known(observation)) continue;
        const auto* content_value = find(observation, "content");
        if (!content_value || !content_value->is_object() ||
            !subset(content_value->as_object(), content_fields)) continue;
        try { validate_authored_observation(step, source.cues); }
        catch (const std::exception&) { continue; }
        const auto* variants = find(content_value->as_object(), "variants");
        if (!variants || !variants->is_array()) continue;
        paths[index] = {};
        for (const auto* anchor : anchors) {
            if (anchor->modality != "text" || anchor->role != "original" ||
                anchor->path.size() < 3 ||
                !std::holds_alternative<std::int64_t>(anchor->path[2])) continue;
            const auto variant_index = std::get<std::int64_t>(anchor->path[2]);
            if (variant_index < 0 || static_cast<std::size_t>(variant_index) >= variants->as_array().size()) continue;
            const auto& variant = variants->as_array()[static_cast<std::size_t>(variant_index)];
            if (!variant.is_object() || !subset(variant.as_object(), variant_fields)) continue;
            const auto metadata = find(variant.as_object(), "authored_metadata");
            if (metadata && !metadata->is_object()) continue;
            paths[index].insert(anchor->path);
        }
    }
    return paths;
}

std::vector<SemanticDeliveredPart> source_text_parts(const SemanticSourceEpisode& source) {
    static const std::set<std::string, std::less<>> outcome_fields{
        "text", "source_family", "task_family", "source_item_id"};
    static const std::set<std::string, std::less<>> sealed_fields{
        "text", "source_family", "task_family", "source_item_id", "schema_version",
        "source_id", "source_revision_receipt", "one_original_source_equals_one_experience",
        "generated_frames_count_as_new_experience"};
    static const std::set<std::string, std::less<>> summary_fields{
        "summary_id", "source_id", "source_revision", "source_digest", "outcome",
        "source_addresses", "text", "quotes", "profile", "model", "schema", "derived",
        "new_observation_count", "independent_evidence_count", "semantic_authority",
        "persistent_write_authority", "association_cues", "evidence_family",
        "overlap_is_independent_corroboration"};
    std::vector<SemanticDeliveredPart> parts;
    for (std::size_t index = 0; index < source.steps.size(); ++index) {
        const auto& observation_value = source.steps[index].observation;
        if (!observation_value.is_object()) throw std::invalid_argument("semantic_source_adapter_unavailable");
        const auto& observation = observation_value.as_object();
        std::vector<std::tuple<Path, std::string, std::string, std::string>> values;
        if (string_is(find(observation, "schema_version"), authored_media_observation_schema) &&
            string_is(find(observation, "media_kind"), "authored_text")) {
            const auto& variants = observation.at("content").at("variants").as_array();
            for (std::size_t variant = 0; variant < variants.size(); ++variant)
                values.push_back({{std::string("content"), std::string("variants"),
                    static_cast<std::int64_t>(variant), std::string("text")},
                    std::string(variants[variant].at("text").as_string()), "original",
                    "text-" + std::to_string(index) + "-" + std::to_string(variant)});
        } else if ((observation.size() == 1 && observation.contains("text")) ||
                   object_keys(observation) == outcome_fields ||
                   (object_keys(observation) == sealed_fields &&
                    string_is(find(observation, "schema_version"), "rozephine-paper-source-diverse-outcome-v1"))) {
            values.push_back({{std::string("text")}, std::string(observation.at("text").as_string()),
                              "original", "text-" + std::to_string(index)});
        } else if ((string_is(find(observation, "schema"), "rozephine-joint-summary-v2") ||
                    string_is(find(observation, "schema"), "rozephine-joint-summary-v3")) &&
                   boolean_is(find(observation, "derived"), true) &&
                   boolean_is(find(observation, "semantic_authority"), false) &&
                   integer(find(observation, "independent_evidence_count"), "semantic_summary_fields_unavailable") == 0) {
            auto fields = summary_fields;
            if (string_is(find(observation, "schema"), "rozephine-joint-summary-v3")) {
                fields.insert("derivation_method"); fields.insert("reuse_provenance");
            }
            if (object_keys(observation) != fields ||
                !observation.at("quotes").is_array()) throw std::invalid_argument("semantic_summary_fields_unavailable");
            values.push_back({{std::string("text")}, std::string(observation.at("text").as_string()),
                              "summary", "summary-" + std::to_string(index)});
            const auto& quotes = observation.at("quotes").as_array();
            for (std::size_t quote = 0; quote < quotes.size(); ++quote)
                values.push_back({{std::string("quotes"), static_cast<std::int64_t>(quote)},
                    std::string(quotes[quote].as_string()), "summary",
                    "summary-quote-" + std::to_string(index) + "-" + std::to_string(quote)});
        } else throw std::invalid_argument("semantic_source_adapter_unavailable");
        if (values.empty()) throw std::invalid_argument("semantic_source_adapter_unavailable");
        for (auto& [path, value, role, identifier] : values) {
            if (!text(value)) throw std::invalid_argument("semantic_source_text_empty");
            parts.push_back({{std::move(identifier), static_cast<std::int64_t>(index), std::move(path),
                              "text", std::move(role), {0, static_cast<std::int64_t>(utf8_length(value))}, {}, {}},
                             std::move(value), "text/plain"});
        }
    }
    if (parts.empty()) throw std::invalid_argument("semantic_source_adapter_unavailable");
    return parts;
}

std::vector<std::int64_t> declared_text_steps(
    const SemanticSourceEpisode& source, const SemanticEncoding& encoding) {
    std::vector<SemanticDeliveredPart> expected;
    try { expected = source_text_parts(source); }
    catch (const std::exception&) { return {}; }
    std::unordered_set<std::string> used;
    for (const auto& unit : encoding.units) {
        used.insert(unit.anchors.begin(), unit.anchors.end());
        for (const auto& qualifier : unit.qualifiers)
            used.insert(qualifier.anchors.begin(), qualifier.anchors.end());
    }
    using SpanKey = std::tuple<std::int64_t, Path, std::string>;
    std::map<SpanKey, std::vector<std::vector<std::int64_t>>> spans;
    for (const auto& anchor : encoding.anchors)
        if (used.contains(anchor.identifier) && anchor.modality == "text")
            spans[{anchor.step, anchor.path, anchor.role}].push_back(anchor.char_range);
    std::map<std::int64_t, std::vector<std::pair<SemanticAnchor, std::int64_t>>> required;
    for (const auto& part : expected)
        required[part.anchor.step].push_back({part.anchor,
            static_cast<std::int64_t>(utf8_length(std::get<std::string>(part.content)))});
    std::vector<std::int64_t> complete;
    for (const auto& [index, parts] : required) {
        const auto& step = source.steps[static_cast<std::size_t>(index)];
        if (step.observation.is_object() &&
            string_is(find(step.observation.as_object(), "schema_version"), authored_media_observation_schema)) {
            if (!authored_layout_known(step.observation.as_object())) continue;
            try { validate_authored_observation(step, source.cues); }
            catch (const std::exception&) { continue; }
        }
        bool all = true;
        for (const auto& [anchor, extent] : parts) {
            auto intervals = spans[{anchor.step, anchor.path, anchor.role}];
            std::ranges::sort(intervals);
            std::int64_t cursor = 0;
            for (const auto& interval : intervals) {
                if (interval.size() != 2 || interval[0] > cursor) break;
                cursor = std::max(cursor, interval[1]);
            }
            if (cursor != extent) { all = false; break; }
        }
        if (all) complete.push_back(index);
    }
    return complete;
}

}  // namespace

std::map<std::string, std::int64_t, std::less<>> attributable_text_anchors(
    const SemanticSourceEpisode& source, const SemanticEncoding& encoding,
    const SemanticContextByAnchor* provided) {
    const auto owned = provided ? SemanticContextByAnchor{} : input_context_by_anchor(encoding);
    const auto& context = provided ? *provided : owned;
    auto paths = authored_context_paths(source, encoding, context);
    static const std::set<std::string, std::less<>> document_fields{
        "schema_version", "source_family", "task_family", "title", "canonical_url",
        "normalized_document_text", "normalized_text_bytes", "normalized_text_sha256",
        "rendered_document_variants", "body_groups", "character_labels",
        "document_claims_independently_verified", "read_completion_is_growth",
        "redirect_aliases_counted_as_new_experience", "requested_addresses",
        "resolution_failures", "revision_timestamps", "section_headings"};
    static const std::set<std::string, std::less<>> variant_fields{
        "canonical_url", "final_url", "manifest_label", "normalized_document_text",
        "normalized_text_bytes", "normalized_text_sha256", "requested_url"};
    for (std::size_t index = 0; index < source.steps.size(); ++index) {
        const auto& value = source.steps[index].observation;
        if (!value.is_object()) continue;
        const auto& observation = value.as_object();
        if (observation.size() == 1 && observation.contains("text") &&
            std::holds_alternative<std::string>(observation.at("text").storage()))
            paths[static_cast<std::int64_t>(index)] = {Path{std::string("text")}};
        else if (string_is(find(observation, "schema_version"), semantic_document_schema) &&
                 subset(observation, document_fields) &&
                 boolean_is(find(observation, "document_claims_independently_verified"), false) &&
                 boolean_is(find(observation, "read_completion_is_growth"), false) &&
                 integer(find(observation, "redirect_aliases_counted_as_new_experience"), "invalid document") == 0) {
            const auto* variants = find(observation, "rendered_document_variants");
            if (!variants || !variants->is_array() || std::ranges::any_of(variants->as_array(), [&](const auto& variant) {
                return !variant.is_object() || !subset(variant.as_object(), variant_fields);
            })) continue;
            auto& step_paths = paths[static_cast<std::int64_t>(index)];
            step_paths.insert({std::string("normalized_document_text")});
            for (std::size_t variant = 0; variant < variants->as_array().size(); ++variant)
                step_paths.insert({std::string("rendered_document_variants"),
                                   static_cast<std::int64_t>(variant),
                                   std::string("normalized_document_text")});
        }
    }
    std::map<std::string, std::int64_t, std::less<>> result;
    for (const auto& anchor : encoding.anchors)
        if (anchor.modality == "text" && anchor.role == "original" &&
            paths.contains(anchor.step) && paths.at(anchor.step).contains(anchor.path))
            result.emplace(anchor.identifier, anchor.step);
    return result;
}

std::vector<std::int64_t> interpreted_input_steps(
    const SemanticSourceEpisode& source, const SemanticEncoding& encoding) {
    if (!encoding.unresolved.empty() || encoding.units.empty()) return {};
    std::unordered_set<std::string> used;
    for (const auto& unit : encoding.units) {
        used.insert(unit.anchors.begin(), unit.anchors.end());
        for (const auto& qualifier : unit.qualifiers)
            used.insert(qualifier.anchors.begin(), qualifier.anchors.end());
    }
    std::map<std::int64_t, std::set<std::string, std::less<>>> delivered;
    using RangeKey = std::pair<std::int64_t, std::string>;
    std::map<RangeKey, std::vector<std::vector<std::int64_t>>> ranges;
    for (const auto& anchor : encoding.anchors) {
        if (!used.contains(anchor.identifier) || anchor.path.size() != 1 ||
            !std::holds_alternative<std::string>(anchor.path[0]) || anchor.step < 0 ||
            static_cast<std::size_t>(anchor.step) >= source.steps.size()) continue;
        const auto& name = std::get<std::string>(anchor.path[0]);
        const auto& observation = source.steps[static_cast<std::size_t>(anchor.step)].observation.as_object();
        const auto& value = observation.at(name);
        if (anchor.modality == "text") ranges[{anchor.step, name}].push_back(anchor.char_range);
        else if (anchor.modality == "image") {
            const auto width = integer(find(value.as_object(), "width"), "native_image_dimensions_required");
            const auto height = integer(find(value.as_object(), "height"), "native_image_dimensions_required");
            if (anchor.region.empty() || anchor.region == std::vector<std::int64_t>{0, 0, width, height})
                delivered[anchor.step].insert(name);
        } else if (anchor.modality == "audio" || anchor.modality == "video") {
            bool full_region = anchor.modality == "audio" || anchor.region.empty();
            if (!full_region) {
                const auto width = integer(find(value.as_object(), "width"), "native_image_dimensions_required");
                const auto height = integer(find(value.as_object(), "height"), "native_image_dimensions_required");
                full_region = anchor.region == std::vector<std::int64_t>{0, 0, width, height};
            }
            if (full_region) ranges[{anchor.step, name}].push_back(anchor.time_ns);
        }
    }
    for (auto& [key, intervals] : ranges) {
        const auto& [step, name] = key;
        const auto& value = source.steps[static_cast<std::size_t>(step)].observation.at(name);
        const auto extent = std::holds_alternative<std::string>(value.storage())
            ? static_cast<std::int64_t>(utf8_length(value.as_string()))
            : integer(find(value.as_object(), "duration_ns"), "integer_nanosecond_interval_required");
        std::ranges::sort(intervals);
        std::int64_t cursor = 0;
        for (const auto& interval : intervals) {
            if (interval.size() != 2 || interval[0] > cursor) break;
            cursor = std::max(cursor, interval[1]);
        }
        if (cursor == extent) delivered[step].insert(name);
    }
    std::set<std::int64_t> direct;
    for (const auto& [step, names] : delivered) {
        const auto& observation = source.steps[static_cast<std::size_t>(step)].observation.as_object();
        if (names.size() == observation.size() && std::ranges::all_of(observation, [&](const auto& item) {
                return names.contains(item.first);
            })) direct.insert(step);
    }
    if (direct.size() == source.steps.size()) return {direct.begin(), direct.end()};
    const auto declared = declared_text_steps(source, encoding);
    direct.insert(declared.begin(), declared.end());
    return {direct.begin(), direct.end()};
}

}  // namespace swegca::world
