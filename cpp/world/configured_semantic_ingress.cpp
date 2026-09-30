#include "world/configured_semantic_ingress.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

namespace swegca::world {
namespace {

constexpr std::string_view media_schema =
    "rozephine-authored-media-observation-v1";
constexpr std::string_view summary_schema = "rozephine-joint-summary-v2";
constexpr std::string_view summary_reuse_schema = "rozephine-joint-summary-v3";

const std::set<std::string, std::less<>> summary_fields{
    "summary_id", "source_id", "source_revision", "source_digest",
    "outcome", "source_addresses", "text", "quotes", "profile", "model",
    "schema", "derived", "new_observation_count",
    "independent_evidence_count", "semantic_authority",
    "persistent_write_authority", "association_cues", "evidence_family",
    "overlap_is_independent_corroboration"};
const std::set<std::string, std::less<>> outcome_text_fields{
    "text", "source_family", "task_family", "source_item_id"};
const std::set<std::string, std::less<>> sealed_outcome_fields{
    "text", "source_family", "task_family", "source_item_id",
    "schema_version", "source_id", "source_revision_receipt",
    "one_original_source_equals_one_experience",
    "generated_frames_count_as_new_experience"};

const JsonValue* find(const JsonValue::Object& object, const std::string_view key) {
    const auto item = object.find(key);
    return item == object.end() ? nullptr : &item->second;
}

const std::string* string(const JsonValue::Object& object, const std::string_view key) {
    const auto* value = find(object, key);
    return value ? std::get_if<std::string>(&value->storage()) : nullptr;
}

bool exact_fields(const JsonValue::Object& object,
                  const std::set<std::string, std::less<>>& fields) {
    if (object.size() != fields.size()) return false;
    return std::ranges::all_of(fields, [&](const auto& key) {
        return object.contains(key);
    });
}

bool python_whitespace(const std::uint32_t codepoint) noexcept {
    return (codepoint >= 0x09U && codepoint <= 0x0dU) ||
           (codepoint >= 0x1cU && codepoint <= 0x20U) ||
           codepoint == 0x85U || codepoint == 0xa0U || codepoint == 0x1680U ||
           (codepoint >= 0x2000U && codepoint <= 0x200aU) ||
           codepoint == 0x2028U || codepoint == 0x2029U ||
           codepoint == 0x202fU || codepoint == 0x205fU || codepoint == 0x3000U;
}

std::pair<std::uint32_t, std::size_t> decode_utf8(
    const std::string_view value, const std::size_t offset) {
    const auto first = static_cast<unsigned char>(value[offset]);
    if (first <= 0x7fU) return {first, 1};
    std::size_t width{};
    std::uint32_t codepoint{};
    std::uint32_t minimum{};
    if (first >= 0xc2U && first <= 0xdfU) {
        width = 2; codepoint = first & 0x1fU; minimum = 0x80U;
    } else if (first >= 0xe0U && first <= 0xefU) {
        width = 3; codepoint = first & 0x0fU; minimum = 0x800U;
    } else if (first >= 0xf0U && first <= 0xf4U) {
        width = 4; codepoint = first & 0x07U; minimum = 0x10000U;
    } else {
        throw SourceAdapterUnavailable("semantic_source_text_empty");
    }
    if (width > value.size() - offset)
        throw SourceAdapterUnavailable("semantic_source_text_empty");
    for (std::size_t index = 1; index < width; ++index) {
        const auto byte = static_cast<unsigned char>(value[offset + index]);
        if ((byte & 0xc0U) != 0x80U)
            throw SourceAdapterUnavailable("semantic_source_text_empty");
        codepoint = (codepoint << 6U) | (byte & 0x3fU);
    }
    if (codepoint < minimum || codepoint > 0x10ffffU ||
        (codepoint >= 0xd800U && codepoint <= 0xdfffU))
        throw SourceAdapterUnavailable("semantic_source_text_empty");
    return {codepoint, width};
}

std::int64_t checked_text_length(const std::string_view value) {
    std::size_t offset{};
    std::int64_t length{};
    bool has_nonspace{};
    while (offset < value.size()) {
        const auto [codepoint, width] = decode_utf8(value, offset);
        has_nonspace = has_nonspace || !python_whitespace(codepoint);
        offset += width;
        if (length == std::numeric_limits<std::int64_t>::max())
            throw SourceAdapterUnavailable("semantic_source_text_empty");
        ++length;
    }
    if (!has_nonspace)
        throw SourceAdapterUnavailable("semantic_source_text_empty");
    return length;
}

struct TextValue final {
    std::vector<SemanticPathElement> path;
    std::string text;
    std::string role;
    std::string identifier;
};

std::vector<SemanticDeliveredPart> text_parts(
    const SemanticSourceEpisode& source, const bool authored_only) {
    std::vector<SemanticDeliveredPart> result;
    for (std::size_t index = 0; index < source.steps.size(); ++index) {
        const auto& observation = source.steps[index].observation;
        if (!observation.is_object())
            throw SourceAdapterUnavailable("semantic_source_adapter_unavailable");
        const auto& object = observation.as_object();
        const auto* schema = string(object, "schema_version");
        const auto* media_kind = string(object, "media_kind");
        const bool authored = schema && *schema == media_schema &&
                              media_kind && *media_kind == "authored_text";
        std::vector<TextValue> values;
        if (authored) {
            const auto* content = find(object, "content");
            const auto* variants = content && content->is_object()
                ? find(content->as_object(), "variants") : nullptr;
            if (!variants || !variants->is_array())
                throw SourceAdapterUnavailable("semantic_source_adapter_unavailable");
            for (std::size_t variant = 0; variant < variants->as_array().size(); ++variant) {
                const auto& row = variants->as_array()[variant];
                if (!row.is_object() || !string(row.as_object(), "text"))
                    throw SourceAdapterUnavailable("semantic_source_adapter_unavailable");
                values.push_back({
                    {std::string("content"), std::string("variants"),
                     static_cast<std::int64_t>(variant), std::string("text")},
                    *string(row.as_object(), "text"), "original",
                    "text-" + std::to_string(index) + "-" +
                        std::to_string(variant)});
            }
        } else if (!authored_only &&
                   (exact_fields(object, std::set<std::string, std::less<>>{"text"}) ||
                    exact_fields(object, outcome_text_fields) ||
                    (exact_fields(object, sealed_outcome_fields) && schema &&
                     *schema == "rozephine-paper-source-diverse-outcome-v1"))) {
            const auto* text = string(object, "text");
            if (!text)
                throw SourceAdapterUnavailable("semantic_source_adapter_unavailable");
            values.push_back({{std::string("text")}, *text, "original",
                              "text-" + std::to_string(index)});
        } else if (!authored_only) {
            const auto* derivative_schema = string(object, "schema");
            const auto* derived = find(object, "derived");
            const auto* authority = find(object, "semantic_authority");
            const auto* evidence_count = find(object, "independent_evidence_count");
            const bool matching_schema = derivative_schema &&
                (*derivative_schema == summary_schema ||
                 *derivative_schema == summary_reuse_schema);
            const bool matching_flags = derived &&
                std::get_if<bool>(&derived->storage()) &&
                *std::get_if<bool>(&derived->storage()) && authority &&
                std::get_if<bool>(&authority->storage()) &&
                !*std::get_if<bool>(&authority->storage()) && evidence_count &&
                std::get_if<std::int64_t>(&evidence_count->storage()) &&
                *std::get_if<std::int64_t>(&evidence_count->storage()) == 0;
            if (!matching_schema || !matching_flags)
                throw SourceAdapterUnavailable("semantic_source_adapter_unavailable");
            auto fields = summary_fields;
            if (*derivative_schema == summary_reuse_schema) {
                fields.insert("derivation_method");
                fields.insert("reuse_provenance");
            }
            const auto* quotes = find(object, "quotes");
            const auto* text = string(object, "text");
            if (!exact_fields(object, fields) || !quotes || !quotes->is_array() || !text)
                throw SourceAdapterUnavailable("semantic_summary_fields_unavailable");
            values.push_back({{std::string("text")}, *text, "summary",
                              "summary-" + std::to_string(index)});
            for (std::size_t quote = 0; quote < quotes->as_array().size(); ++quote) {
                const auto* quote_text =
                    std::get_if<std::string>(&quotes->as_array()[quote].storage());
                if (!quote_text)
                    throw SourceAdapterUnavailable("semantic_source_text_empty");
                values.push_back({
                    {std::string("quotes"), static_cast<std::int64_t>(quote)},
                    *quote_text, "summary", "summary-quote-" +
                        std::to_string(index) + "-" + std::to_string(quote)});
            }
        } else {
            throw SourceAdapterUnavailable("semantic_source_adapter_unavailable");
        }
        if (values.empty())
            throw SourceAdapterUnavailable("semantic_source_adapter_unavailable");
        for (auto& value : values) {
            const auto length = checked_text_length(value.text);
            SemanticAnchor anchor{
                std::move(value.identifier), static_cast<std::int64_t>(index),
                std::move(value.path), "text", std::move(value.role),
                {0, length}, {}, {}};
            result.push_back({std::move(anchor), std::move(value.text), "text/plain"});
        }
    }
    if (result.empty())
        throw SourceAdapterUnavailable("semantic_source_adapter_unavailable");
    return result;
}

}  // namespace

std::vector<SemanticDeliveredPart> authored_text_parts(
    const SemanticSourceEpisode& source) {
    return text_parts(source, true);
}

std::vector<SemanticDeliveredPart> source_text_parts(
    const SemanticSourceEpisode& source) {
    return text_parts(source, false);
}

ConfiguredSemanticIngress::ConfiguredSemanticIngress(
    const bool authored_only, ProviderProfile profile, ProviderPost post)
    : authored_only_(authored_only),
      producer_(std::move(profile), {"text"}, std::move(post)) {}

PreparedSemanticBatch ConfiguredSemanticIngress::operator()(
    const std::vector<SemanticSourceEpisode>& episodes) const {
    const SemanticPartsAdapter parts_for = authored_only_
        ? SemanticPartsAdapter(authored_text_parts)
        : SemanticPartsAdapter(source_text_parts);
    return prepare_semantic_batch(
        episodes, parts_for, producer_.profile.model,
        [this](const SemanticEncodingInput& request) {
            return producer_(request);
        });
}

std::optional<ConfiguredSemanticIngress> configure_semantic_ingress(
    const std::optional<JsonValue>& configuration, ProviderPost post) {
    if (!configuration || std::holds_alternative<std::nullptr_t>(
                              configuration->storage()))
        return std::nullopt;
    if (!configuration->is_object())
        throw std::invalid_argument("invalid_offline_semantic_ingress_configuration");
    const auto& object = configuration->as_object();
    const std::set<std::string, std::less<>> expected{
        "enabled", "adapter", "profile"};
    const auto* enabled_value = find(object, "enabled");
    const auto* adapter_value = find(object, "adapter");
    const auto* profile_value = find(object, "profile");
    const auto* enabled = enabled_value
        ? std::get_if<bool>(&enabled_value->storage()) : nullptr;
    const auto* adapter = adapter_value
        ? std::get_if<std::string>(&adapter_value->storage()) : nullptr;
    if (!exact_fields(object, expected) || !enabled || !adapter ||
        (*adapter != "authored-text-v1" && *adapter != "source-text-v1") ||
        !profile_value || !profile_value->is_object())
        throw std::invalid_argument("invalid_offline_semantic_ingress_configuration");
    auto specification = profile_value->as_object();
    std::string name;
    if (const auto item = specification.find("name"); item != specification.end()) {
        if (const auto* text = std::get_if<std::string>(&item->second.storage()))
            name = *text;
        specification.erase(item);
    }
    auto profile = ProviderProfile::from_object(std::move(name), specification);
    ConfiguredSemanticIngress ingress(
        *adapter == "authored-text-v1", std::move(profile), std::move(post));
    if (!*enabled) return std::nullopt;
    return ingress;
}

}  // namespace swegca::world
