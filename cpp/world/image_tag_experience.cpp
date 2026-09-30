#include "world/image_tag_experience.hpp"

#include "swegca_architecture/sha256.hpp"
#include "world/unicode_nfkc.hpp"
#include "world/unicode_word_data.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

namespace swegca::world {
namespace {

[[nodiscard]] const JsonValue* find(
    const JsonValue::Object& object, const std::string_view key) noexcept {
    const auto iterator = object.find(key);
    return iterator == object.end() ? nullptr : &iterator->second;
}

[[nodiscard]] bool text(const JsonValue* value) noexcept {
    if (value == nullptr || !std::holds_alternative<std::string>(value->storage())) return false;
    const auto& string = std::get<std::string>(value->storage());
    return std::any_of(string.begin(), string.end(), [](const unsigned char byte) {
        return byte != ' ' && byte != '\t' && byte != '\n' && byte != '\r' && byte != '\f' && byte != '\v';
    });
}

[[nodiscard]] bool exact_bool(const JsonValue* value, const bool expected) noexcept {
    const auto* item = value == nullptr ? nullptr : std::get_if<bool>(&value->storage());
    return item != nullptr && *item == expected;
}

[[nodiscard]] const JsonValue::Object& object(const JsonValue* value, const char* message) {
    if (value == nullptr || !value->is_object()) throw std::invalid_argument(message);
    return value->as_object();
}

[[nodiscard]] const JsonValue::Array& array(const JsonValue* value, const char* message) {
    if (value == nullptr || !value->is_array()) throw std::invalid_argument(message);
    return value->as_array();
}

[[nodiscard]] std::int64_t integer(const JsonValue* value, const char* message) {
    if (value == nullptr) throw std::invalid_argument(message);
    const auto* item = std::get_if<std::int64_t>(&value->storage());
    if (item == nullptr) throw std::invalid_argument(message);
    return *item;
}

[[nodiscard]] double number(const JsonValue* value, const char* message) {
    if (value == nullptr) throw std::invalid_argument(message);
    if (const auto* item = std::get_if<double>(&value->storage())) return *item;
    if (const auto* item = std::get_if<std::int64_t>(&value->storage())) {
        return static_cast<double>(*item);
    }
    throw std::invalid_argument(message);
}

[[nodiscard]] bool is_sha256(const std::string_view value) noexcept {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](const char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

[[nodiscard]] std::string json_string(const std::string_view value) {
    // The pinned Python value is a Unicode str. Reject byte sequences that
    // could not represent that input instead of hashing an impossible row.
    (void)unicode_casefold(value);
    std::string result{"\""};
    for (std::size_t at = 0; at != value.size();) {
        const auto byte = static_cast<unsigned char>(value[at]);
        if (byte < 0x20U) {
            switch (byte) {
            case '\b': result += "\\b"; ++at; continue;
            case '\t': result += "\\t"; ++at; continue;
            case '\n': result += "\\n"; ++at; continue;
            case '\f': result += "\\f"; ++at; continue;
            case '\r': result += "\\r"; ++at; continue;
            default: break;
            }
            constexpr char digits[] = "0123456789abcdef";
            result += "\\u00";
            result.push_back(digits[byte >> 4U]);
            result.push_back(digits[byte & 15U]);
            ++at;
            continue;
        }
        if (byte == '"' || byte == '\\') result.push_back('\\');
        result.push_back(static_cast<char>(byte));
        ++at;
    }
    result.push_back('"');
    return result;
}

[[nodiscard]] std::string python_float(const double value) {
    // json.dumps defaults to allow_nan=True.
    if (std::isnan(value)) return "NaN";
    if (std::isinf(value)) return std::signbit(value) ? "-Infinity" : "Infinity";
    char storage[64]{};
    const auto [end, error] = std::to_chars(
        std::begin(storage), std::end(storage), value, std::chars_format::general);
    if (error != std::errc{}) throw std::runtime_error("cannot encode canonical JSON number");
    std::string result(storage, end);
    const auto exponent_at = result.find('e');
    if (exponent_at == std::string::npos) {
        if (result.find('.') == std::string::npos) result += ".0";
        return result;
    }
    auto exponent_text = std::string_view(result).substr(exponent_at + 1);
    bool negative_exponent = false;
    if (!exponent_text.empty() && (exponent_text.front() == '+' || exponent_text.front() == '-')) {
        negative_exponent = exponent_text.front() == '-';
        exponent_text.remove_prefix(1);
    }
    unsigned magnitude = 0;
    const auto parsed = std::from_chars(
        exponent_text.data(), exponent_text.data() + exponent_text.size(), magnitude);
    if (exponent_text.empty() || parsed.ec != std::errc{} ||
        parsed.ptr != exponent_text.data() + exponent_text.size() ||
        magnitude > static_cast<unsigned>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("cannot normalize canonical JSON exponent");
    }
    const int exponent = negative_exponent ? -static_cast<int>(magnitude) : static_cast<int>(magnitude);
    if (exponent >= -4 && exponent < 16) {
        const bool negative = result.front() == '-';
        const auto mantissa_begin = negative ? 1U : 0U;
        std::string digits;
        for (std::size_t index = mantissa_begin; index < exponent_at; ++index) {
            if (result[index] != '.') digits.push_back(result[index]);
        }
        const auto decimal = std::int64_t{1} + exponent;
        std::string fixed = negative ? "-" : "";
        if (decimal <= 0) {
            fixed += "0.";
            fixed.append(static_cast<std::size_t>(-decimal), '0');
            fixed += digits;
        } else if (static_cast<std::size_t>(decimal) >= digits.size()) {
            fixed += digits;
            fixed.append(static_cast<std::size_t>(decimal) - digits.size(), '0');
            fixed += ".0";
        } else {
            fixed.append(digits, 0, static_cast<std::size_t>(decimal));
            fixed.push_back('.');
            fixed.append(digits, static_cast<std::size_t>(decimal), std::string::npos);
        }
        return fixed;
    }
    std::string scientific = result.substr(0, exponent_at + 1);
    scientific.push_back(exponent < 0 ? '-' : '+');
    const auto unsigned_exponent = static_cast<unsigned>(
        exponent < 0 ? -static_cast<long long>(exponent) : exponent);
    char digits[32]{};
    const auto encoded = std::to_chars(std::begin(digits), std::end(digits), unsigned_exponent);
    const auto count = static_cast<std::size_t>(encoded.ptr - digits);
    if (count < 2) scientific.push_back('0');
    scientific.append(digits, encoded.ptr);
    return scientific;
}

[[nodiscard]] std::string canonical_json(const JsonValue& value) {
    const auto& storage = value.storage();
    if (std::holds_alternative<std::nullptr_t>(storage)) return "null";
    if (const auto* item = std::get_if<bool>(&storage)) return *item ? "true" : "false";
    if (const auto* item = std::get_if<std::int64_t>(&storage)) return std::to_string(*item);
    if (const auto* item = std::get_if<JsonInteger>(&storage)) return item->value;
    if (const auto* item = std::get_if<double>(&storage)) return python_float(*item);
    if (const auto* item = std::get_if<std::string>(&storage)) return json_string(*item);
    if (const auto* items = std::get_if<JsonValue::Array>(&storage)) {
        std::string result{"["};
        for (std::size_t index = 0; index != items->size(); ++index) {
            if (index != 0) result.push_back(',');
            result += canonical_json((*items)[index]);
        }
        result.push_back(']');
        return result;
    }
    std::string result{"{"};
    bool first = true;
    for (const auto& [key, item] : std::get<JsonValue::Object>(storage)) {
        if (!first) result.push_back(',');
        first = false;
        result += json_string(key) + ":" + canonical_json(item);
    }
    result.push_back('}');
    return result;
}

[[nodiscard]] std::string hex(const architecture::DigestBytes& digest) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result(digest.size() * 2, '0');
    for (std::size_t index = 0; index != digest.size(); ++index) {
        const auto byte = std::to_integer<unsigned>(digest[index]);
        result[index * 2] = digits[byte >> 4U];
        result[index * 2 + 1] = digits[byte & 15U];
    }
    return result;
}

[[nodiscard]] std::string sha256(const std::string_view bytes) {
    architecture::Sha256 digest;
    digest.update(bytes);
    return hex(digest.finish());
}

struct Point final {
    char32_t value{};
    std::size_t bytes{};
};

[[nodiscard]] Point point_at(const std::string_view input, const std::size_t at) {
    const auto lead = static_cast<unsigned char>(input[at]);
    if (lead < 0x80U) return {lead, 1};
    std::size_t count = 0;
    char32_t point = 0;
    char32_t minimum = 0;
    if ((lead & 0xe0U) == 0xc0U) { count = 2; point = lead & 0x1fU; minimum = 0x80; }
    else if ((lead & 0xf0U) == 0xe0U) { count = 3; point = lead & 0x0fU; minimum = 0x800; }
    else if ((lead & 0xf8U) == 0xf0U) { count = 4; point = lead & 0x07U; minimum = 0x10000; }
    else throw std::invalid_argument("image tag text contains invalid UTF-8");
    if (count > input.size() - at) throw std::invalid_argument("image tag text contains truncated UTF-8");
    for (std::size_t offset = 1; offset != count; ++offset) {
        const auto next = static_cast<unsigned char>(input[at + offset]);
        if ((next & 0xc0U) != 0x80U) throw std::invalid_argument("image tag text contains invalid UTF-8");
        point = (point << 6U) | (next & 0x3fU);
    }
    if (point < minimum || point > 0x10ffffU || (point >= 0xd800U && point <= 0xdfffU)) {
        throw std::invalid_argument("image tag text contains invalid UTF-8");
    }
    return {point, count};
}

[[nodiscard]] bool lexical_letter(const char32_t point) noexcept {
    return (point >= U'A' && point <= U'Z') || (point >= U'a' && point <= U'z') ||
        (point >= 0xac00 && point <= 0xd7a3) || (point >= 0x3041 && point <= 0x3096) ||
        (point >= 0x30a1 && point <= 0x30fa) || (point >= 0x4e00 && point <= 0x9fff);
}

[[nodiscard]] bool lexical_tail(const char32_t point) noexcept {
    return lexical_letter(point) || (point >= U'0' && point <= U'9');
}

[[nodiscard]] std::vector<std::string> lexical_tokens(std::string names) {
    std::replace(names.begin(), names.end(), '_', ' ');
    std::vector<std::string> result;
    for (std::size_t at = 0; at < names.size();) {
        const auto first = point_at(names, at);
        if (!lexical_letter(first.value)) { at += first.bytes; continue; }
        const auto start = at;
        at += first.bytes;
        std::size_t length = 1;
        while (at < names.size() && length < 64) {
            const auto next = point_at(names, at);
            if (!lexical_tail(next.value)) break;
            at += next.bytes;
            ++length;
        }
        if (length >= 2) result.push_back(unicode_casefold(names.substr(start, at - start)));
        while (at < names.size()) {
            const auto next = point_at(names, at);
            if (!lexical_tail(next.value)) break;
            at += next.bytes;
        }
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

[[nodiscard]] bool conditioning_word(const char32_t point) noexcept {
    const auto iterator = std::lower_bound(
        unicode_word_data::ranges.begin(), unicode_word_data::ranges.end(), point,
        [](const unicode_word_data::Range range, const char32_t value) {
            return range.last < value;
        });
    return iterator != unicode_word_data::ranges.end() && iterator->first <= point;
}

[[nodiscard]] std::vector<std::string> conditioning_tokens(const std::string_view input) {
    std::vector<std::string> result;
    for (std::size_t at = 0; at < input.size();) {
        const auto first = point_at(input, at);
        if (!conditioning_word(first.value)) { at += first.bytes; continue; }
        const auto start = at;
        at += first.bytes;
        std::size_t length = 1;
        while (at < input.size() && length < 64) {
            const auto next = point_at(input, at);
            if (!conditioning_word(next.value)) break;
            at += next.bytes;
            ++length;
        }
        if (length >= 2) result.push_back(unicode_casefold(input.substr(start, at - start)));
        while (at < input.size()) {
            const auto next = point_at(input, at);
            if (!conditioning_word(next.value)) break;
            at += next.bytes;
        }
    }
    return result;
}

void append_unique(std::vector<std::string>& values, std::string value) {
    if (std::find(values.begin(), values.end(), value) == values.end()) {
        values.push_back(std::move(value));
    }
}

[[nodiscard]] bool truthy(const JsonValue* value) noexcept {
    if (value == nullptr || std::holds_alternative<std::nullptr_t>(value->storage())) return false;
    if (const auto* item = std::get_if<bool>(&value->storage())) return *item;
    if (const auto* item = std::get_if<std::int64_t>(&value->storage())) return *item != 0;
    if (const auto* item = std::get_if<JsonInteger>(&value->storage()))
        return item->value != "0";
    if (const auto* item = std::get_if<double>(&value->storage())) return *item != 0.0;
    if (const auto* item = std::get_if<std::string>(&value->storage())) return !item->empty();
    if (const auto* item = std::get_if<JsonValue::Array>(&value->storage())) return !item->empty();
    return !std::get<JsonValue::Object>(value->storage()).empty();
}

}  // namespace

JsonValue image_tag_experience(JsonValue::Object content, const std::string_view source_family,
                               const std::int64_t observed_at_unix_ns) {
    const auto* source_value = find(content, "source_sha256");
    const auto* address_value = find(content, "source_address");
    if (!text(source_value) || !text(address_value)) {
        throw std::invalid_argument("actual image/tag observation binding required");
    }
    const std::string source_id(source_value->as_string());
    const std::string source_address(address_value->as_string());
    const auto revision = sha256(canonical_json(JsonValue(content)));
    const auto query = "image-source:" + source_id;

    std::vector<std::string> cues;
    append_unique(cues, query);
    append_unique(cues, "source-content-sha256:" + source_id);
    append_unique(cues, "media-kind:image_tag_observation");
    std::string joined_names;
    if (const auto* tags = find(content, "tags")) {
        for (const auto& value : array(tags, "image tags must be an array")) {
            const auto& tag = object(&value, "image tag must be an object");
            const auto* name = find(tag, "name");
            if (text(name)) {
                append_unique(cues, std::string(name->as_string()));
                if (!joined_names.empty()) joined_names.push_back(' ');
                joined_names += name->as_string();
            }
        }
    }
    for (auto token : lexical_tokens(joined_names)) append_unique(cues, std::move(token));

    if (const auto* audio_value = find(content, "concurrent_system_audio")) {
        const auto& audio = object(audio_value, "concurrent system audio must be an object");
        if (truthy(find(audio, "segment_count"))) {
            append_unique(cues, "media-kind:system_audio");
            append_unique(cues, "시청각");
            append_unique(cues, "시스템소리");
            append_unique(cues, "audio-feature-observation");
        }
    }
    if (const auto* generation_value = find(content, "generation_conditioning")) {
        const auto& generation = object(generation_value, "generation conditioning must be an object");
        const auto* output = find(generation, "output_sha256");
        if (!text(output) || output->as_string() != source_id ||
            !exact_bool(find(generation, "intent_is_observed_pixel_truth"), false) ||
            !exact_bool(find(generation, "conditioning_used_as_tag_ground_truth"), false)) {
            throw std::invalid_argument("generation conditioning must remain source-bound non-truth evidence");
        }
        const auto& conditioning = object(find(generation, "conditioning"),
            "generation conditioning payload must be an object");
        std::string words;
        for (const auto key : {"prompt", "negative_prompt", "checkpoint"}) {
            if (const auto* item = find(conditioning, key); item != nullptr) {
                if (!words.empty()) words.push_back(' ');
                if (const auto* string = std::get_if<std::string>(&item->storage())) words += *string;
                else if (const auto* integer_value = std::get_if<std::int64_t>(&item->storage())) words += std::to_string(*integer_value);
                else if (const auto* number_value = std::get_if<double>(&item->storage())) words += python_float(*number_value);
                else if (const auto* boolean = std::get_if<bool>(&item->storage())) words += *boolean ? "True" : "False";
                else if (std::holds_alternative<std::nullptr_t>(item->storage())) words += "None";
                else words += canonical_json(*item);
            }
        }
        const auto* manifest = find(generation, "manifest_sha256");
        if (!text(manifest)) throw std::invalid_argument("generation manifest binding required");
        append_unique(cues, "generation-conditioning:" + std::string(manifest->as_string()));
        append_unique(cues, "source-family:anima-local-generation");
        for (auto token : conditioning_tokens(words)) {
            append_unique(cues, "conditioning:" + std::move(token));
        }
    }

    JsonValue::Array cue_values;
    cue_values.reserve(cues.size());
    for (const auto& cue : cues) cue_values.emplace_back(cue);
    JsonValue::Object observation{
        {"schema_version", authored_media_observation_schema}, {"source_id", source_id},
        {"source_item_id", source_id}, {"source_revision_receipt", revision},
        {"source_family", source_family}, {"task_family", "source-bound-image-tag-observation"},
        {"observation_query", query}, {"media_kind", "image_tag_observation"},
        {"content", JsonValue(std::move(content))}, {"source_origin", "user_authorized_local_image"},
        {"observed_at_unix_ns", observed_at_unix_ns}, {"actual_world_outcomes_claimed", 0},
        {"source_observation_count", 1}, {"generated_views_are_new_events", false},
        {"semantics_verified", false}, {"growth_claimed", false},
        {"translation_variants_are_new_events", false}};
    JsonValue::Object step{
        {"phase", "observation_attempt_outcome"}, {"observation", JsonValue(std::move(observation))},
        {"relations", JsonValue::Array{}},
        {"judgment", "Image decoded; WD14 descriptions are uncertain proposals, not verified identity."},
        {"outcome", "pending"}, {"evidence_refs", JsonValue::Array{source_address}}};
    JsonValue episode(JsonValue::Object{
        {"schema_version", "rozephine-organized-experience-episode-v1"},
        {"episode_id", "experience:" + source_id}, {"cues", std::move(cue_values)},
        {"step", JsonValue(std::move(step))}, {"source_addresses", JsonValue::Array{source_address}},
        {"revision", revision}, {"verification_state", "specialist_organized_pending_vrs"}});
    validate_image_tag_experience(episode);
    return episode;
}

void validate_image_tag_experience(const JsonValue& episode) {
    const auto& root = object(&episode, "image tag episode must be an object");
    const auto& cues_array = array(find(root, "cues"), "image tag cues must be an array");
    std::vector<std::string_view> cues;
    cues.reserve(cues_array.size());
    for (const auto& cue : cues_array) {
        if (!std::holds_alternative<std::string>(cue.storage())) {
            throw std::invalid_argument("image tag cue must be text");
        }
        cues.push_back(cue.as_string());
    }
    const auto& step = object(find(root, "step"), "image tag step must be an object");
    const auto& relations = array(find(step, "relations"), "image tag relations must be an array");
    const auto& observation = object(find(step, "observation"), "image tag observation must be an object");
    const auto query = find(observation, "observation_query");
    const auto contains_query = text(query) &&
        std::find(cues.begin(), cues.end(), query->as_string()) != cues.end();
    if (!text(find(observation, "schema_version")) ||
        find(observation, "schema_version")->as_string() != authored_media_observation_schema ||
        !text(find(step, "phase")) || find(step, "phase")->as_string() != "observation_attempt_outcome" ||
        !text(find(step, "outcome")) || find(step, "outcome")->as_string() != "pending" ||
        !exact_bool(find(observation, "semantics_verified"), false) ||
        !exact_bool(find(observation, "growth_claimed"), false) ||
        !exact_bool(find(observation, "translation_variants_are_new_events"), false) || !contains_query) {
        throw std::invalid_argument("media observation/authority boundary changed");
    }
    const auto forbidden_cue = [](const std::string_view cue) {
        return cue.starts_with("actual-relation:") || cue.starts_with("phase7-temporal-profile-v2:");
    };
    if (std::any_of(cues.begin(), cues.end(), forbidden_cue) ||
        std::any_of(relations.begin(), relations.end(), [&](const JsonValue& relation) {
            return std::holds_alternative<std::string>(relation.storage()) && forbidden_cue(relation.as_string());
        })) {
        throw std::invalid_argument("media observation/authority boundary changed");
    }
    for (const auto required : {"source_id", "source_revision_receipt", "source_item_id",
                                "source_family", "task_family"}) {
        if (!text(find(observation, required))) throw std::invalid_argument("media source binding incomplete");
    }
    const auto source_id = find(observation, "source_id")->as_string();
    if (!is_sha256(find(observation, "source_revision_receipt")->as_string())) {
        throw std::invalid_argument("media observation revision must be content addressed");
    }
    if (!text(query) || query->as_string() != "image-source:" + std::string(source_id) ||
        !text(find(observation, "media_kind")) ||
        find(observation, "media_kind")->as_string() != "image_tag_observation" ||
        !text(find(observation, "source_origin")) ||
        find(observation, "source_origin")->as_string() != "user_authorized_local_image" ||
        integer(find(observation, "actual_world_outcomes_claimed"), "invalid world outcome count") != 0 ||
        integer(find(observation, "source_observation_count"), "invalid observation count") != 1 ||
        !exact_bool(find(observation, "generated_views_are_new_events"), false) ||
        integer(find(observation, "observed_at_unix_ns"), "invalid observation time") <= 0) {
        throw std::invalid_argument("image observation provenance boundary changed");
    }
    const auto& content = object(find(observation, "content"), "actual observed media content required");
    if (!text(find(content, "actual_decode_outcome")) ||
        find(content, "actual_decode_outcome")->as_string() != "image_decoded_and_exhaustively_tiled_embedded" ||
        !exact_bool(find(content, "tag_semantics_verified"), false) ||
        !exact_bool(find(content, "generation_prompt_used"), false) || !text(find(content, "source_address")) ||
        !text(find(content, "source_sha256")) || !is_sha256(find(content, "source_sha256")->as_string())) {
        throw std::invalid_argument("actual image/tag observation binding required");
    }
    const auto dimensions = [&](const char* key) {
        const auto& values = array(find(content, key), "image dimensions must be arrays");
        if (values.size() != 2) throw std::invalid_argument("aspect-preserving high-resolution image contract changed");
        const auto width = integer(&values[0], "image dimension must be an integer");
        const auto height = integer(&values[1], "image dimension must be an integer");
        if (width <= 0 || height <= 0) throw std::invalid_argument("aspect-preserving high-resolution image contract changed");
        return std::pair{width, height};
    };
    const auto [native_width, native_height] = dimensions("native_dimensions");
    const auto [delivered_width, delivered_height] = dimensions("delivered_dimensions");
    const auto native_ratio = static_cast<double>(native_width) / static_cast<double>(native_height);
    const auto delivered_ratio = static_cast<double>(delivered_width) / static_cast<double>(delivered_height);
    const auto delivered_pixels = static_cast<long double>(delivered_width) *
                                  static_cast<long double>(delivered_height);
    const auto native_pixels = static_cast<long double>(native_width) *
                               static_cast<long double>(native_height);
    if (std::abs(delivered_ratio - native_ratio) > 0.005 * std::max(1.0, native_ratio) ||
        delivered_pixels > 1'005'000 || (native_pixels > 1'000'000 && delivered_pixels < 995'000)) {
        throw std::invalid_argument("aspect-preserving high-resolution image contract changed");
    }
    const auto& views = array(find(content, "views"), "actual visual features required");
    if (views.empty() || !truthy(find(content, "vision_model")) || !truthy(find(content, "tagger_model"))) {
        throw std::invalid_argument("actual visual features and both model bindings required");
    }
    for (const auto& value : views) {
        const auto& view = object(&value, "image view must be an object");
        const auto& vector = array(find(view, "embedding"), "image embedding must be an array");
        const auto& region = array(find(view, "source_region_original_xywh"), "image region must be an array");
        const auto& tensor = array(find(view, "model_tensor_dimensions"), "model tensor dimensions must be an array");
        if (vector.size() != 384 || region.size() != 4 || tensor.size() != 2) {
            throw std::invalid_argument("finite visual features with native-coordinate regions required");
        }
        for (const auto& element : vector) {
            if (!std::isfinite(number(&element, "embedding value must be numeric"))) {
                throw std::invalid_argument("finite visual features with native-coordinate regions required");
            }
        }
        const auto x = integer(&region[0], "image region must use integers");
        const auto y = integer(&region[1], "image region must use integers");
        const auto width = integer(&region[2], "image region must use integers");
        const auto height = integer(&region[3], "image region must use integers");
        if (x < 0 || y < 0 || width <= 0 || height <= 0 || x > native_width || y > native_height ||
            width > native_width - x || height > native_height - y ||
            integer(&tensor[0], "invalid model tensor") != 518 ||
            integer(&tensor[1], "invalid model tensor") != 518) {
            throw std::invalid_argument("finite visual features with native-coordinate regions required");
        }
    }
    const auto threshold = number(find(content, "tag_threshold"), "tag threshold must be explicit");
    if (!std::isfinite(threshold) || threshold < 0.0 || threshold > 1.0) {
        throw std::invalid_argument("tag threshold must be explicit");
    }
    if (const auto* tags_value = find(content, "tags")) {
        for (const auto& value : array(tags_value, "image tags must be an array")) {
            const auto& tag = object(&value, "image tag must be an object");
            const auto category = integer(find(tag, "category"), "tag category must be an integer");
            const auto score = number(find(tag, "score"), "tag score must be numeric");
            if (!text(find(tag, "name")) || (category != 0 && category != 4 && category != 9) ||
                !std::isfinite(score) || score < threshold || score > 1.0) {
                throw std::invalid_argument("uncertain tag score/category binding required");
            }
        }
    }
}

void validate_media_observation(
    const SemanticMemoryStep& step, const std::vector<std::string>& cues) {
    const auto& observation = object(&step.observation,
        "media observation/authority boundary changed");
    const auto* schema = find(observation, "schema_version");
    const auto* outcome = &step.outcome;
    const auto* query = find(observation, "observation_query");
    const auto query_text = text(query) ? std::string(query->as_string()) : std::string{};
    const bool query_present = std::ranges::find(cues, query_text) != cues.end();
    const bool query_kind = query_text.starts_with("steam-game:") ||
        query_text.starts_with("authored-source:") ||
        query_text.starts_with("image-source:");
    const auto forbidden = [](const std::string_view cue) {
        return cue.starts_with("actual-relation:") ||
               cue.starts_with("phase7-temporal-profile-v2:");
    };
    if (!text(schema) || schema->as_string() != authored_media_observation_schema ||
        step.phase != "observation_attempt_outcome" ||
        (*outcome != "pending" && *outcome != "uncertain" && *outcome != "failure") ||
        !exact_bool(find(observation, "semantics_verified"), false) ||
        !exact_bool(find(observation, "growth_claimed"), false) ||
        !exact_bool(find(observation, "translation_variants_are_new_events"), false) ||
        !query_present || !query_kind || std::ranges::any_of(cues, forbidden) ||
        std::ranges::any_of(step.relations, forbidden))
        throw std::invalid_argument("media observation/authority boundary changed");
    for (const auto required : {"source_id", "source_revision_receipt",
                                "source_item_id", "source_family", "task_family"})
        if (!text(find(observation, required)))
            throw std::invalid_argument("media source binding incomplete");
    if (!is_sha256(find(observation, "source_revision_receipt")->as_string()))
        throw std::invalid_argument(
            "media observation revision must be content addressed");
    const auto* kind_value = find(observation, "media_kind");
    const std::string kind = text(kind_value)
        ? std::string(kind_value->as_string()) : std::string{};
    const auto& content = object(find(observation, "content"),
        "actual observed media content required");
    if (content.empty())
        throw std::invalid_argument("actual observed media content required");
    const auto source_id = std::string(find(observation, "source_id")->as_string());
    if (query_text.starts_with("image-source:")) {
        if ((kind != "image_tag_observation" && kind != "image_caption_observation" &&
             kind != "source_exception") || query_text != "image-source:" + source_id ||
            !text(find(observation, "source_origin")) ||
            find(observation, "source_origin")->as_string() != "user_authorized_local_image" ||
            integer(find(observation, "actual_world_outcomes_claimed"), "image provenance") != 0 ||
            integer(find(observation, "source_observation_count"), "image provenance") != 1 ||
            !exact_bool(find(observation, "generated_views_are_new_events"), false) ||
            integer(find(observation, "observed_at_unix_ns"), "image provenance") <= 0)
            throw std::invalid_argument("image observation provenance boundary changed");
    }
    if (query_text.starts_with("authored-source:")) {
        static const std::set<std::string, std::less<>> source_types{
            "codex_session_record", "user_statement", "public_source_paraphrase",
            "public_source_text", "codex_authored_teaching", "model_authored_teaching"};
        static const std::set<std::string, std::less<>> origins{
            "conversation", "public_web", "codex", "model_specialist"};
        const auto* source_type_value = find(observation, "source_type");
        const auto* source_origin_value = find(observation, "source_origin");
        const auto source_type = text(source_type_value)
            ? std::string(source_type_value->as_string()) : std::string{};
        const auto source_origin = text(source_origin_value)
            ? std::string(source_origin_value->as_string()) : std::string{};
        if (kind != "authored_text" || query_text != "authored-source:" + source_id ||
            !source_types.contains(source_type) || !origins.contains(source_origin) ||
            integer(find(observation, "actual_world_outcomes_claimed"), "authored provenance") != 0 ||
            integer(find(observation, "source_observation_count"), "authored provenance") != 1 ||
            !exact_bool(find(observation, "synthetic_examples_count_as_new_experience"), false) ||
            integer(find(observation, "observed_at_unix_ns"), "authored provenance") <= 0)
            throw std::invalid_argument("authored explanation provenance boundary changed");
        const std::map<std::string, std::string, std::less<>> expected{
            {"user_statement", "conversation"}, {"public_source_paraphrase", "public_web"},
            {"public_source_text", "public_web"}, {"codex_session_record", "codex"},
            {"codex_authored_teaching", "codex"},
            {"model_authored_teaching", "model_specialist"}};
        if (expected.at(source_type) != source_origin)
            throw std::invalid_argument("authored explanation source misclassified");
        if (source_type == "model_authored_teaching") {
            const auto* provenance_value = find(content, "model_provenance");
            const auto& provenance = object(provenance_value,
                "model teaching requires explicit non-authoritative provenance");
            if (!text(find(provenance, "model")) || !text(find(provenance, "request_id")) ||
                !text(find(provenance, "receipt_sha256")) ||
                !is_sha256(find(provenance, "receipt_sha256")->as_string()) ||
                !exact_bool(find(provenance, "proposal_only"), true) ||
                !exact_bool(find(provenance, "examples_are_synthetic"), true))
                throw std::invalid_argument(
                    "model teaching requires explicit non-authoritative provenance");
        }
        if (source_type == "public_source_paraphrase" ||
            source_type == "public_source_text") {
            const auto* identity = find(observation, "source_identity");
            if (!text(identity) || !identity->as_string().starts_with("https://") ||
                identity->as_string().size() <= 8)
                throw std::invalid_argument("public explanation requires actual source URL");
        }
        if (source_type == "public_source_text" || source_type == "codex_session_record") {
            const auto& provenance = object(find(content, "source_document"),
                "licensed original text document binding required");
            if (!truthy(find(provenance, "license")) ||
                !truthy(find(provenance, "identity")) ||
                !text(find(provenance, "sha256")) ||
                !is_sha256(find(provenance, "sha256")->as_string()) ||
                !exact_bool(find(provenance,
                    "fragments_are_independent_outcomes"), false))
                throw std::invalid_argument(
                    "licensed original text document binding required");
        }
    }
    const auto dimensions = [&](const char* key, const char* error) {
        const auto& values = array(find(content, key), error);
        if (values.size() != 2) throw std::invalid_argument(error);
        const auto width = integer(&values[0], error);
        const auto height = integer(&values[1], error);
        if (width <= 0 || height <= 0) throw std::invalid_argument(error);
        return std::pair{width, height};
    };
    if (kind == "authored_text") {
        const auto& variants = array(find(content, "variants"),
            "authored text variants required");
        if (variants.empty()) throw std::invalid_argument("authored text variants required");
        for (const auto& value : variants) {
            const auto& row = object(&value,
                "text content/language/source binding incomplete");
            if (!text(find(row, "text")) || !truthy(find(row, "source_address")) ||
                !truthy(find(row, "language")) || !text(find(row, "source_sha256")) ||
                !is_sha256(find(row, "source_sha256")->as_string()))
                throw std::invalid_argument(
                    "text content/language/source binding incomplete");
        }
    } else if (kind == "image_caption_observation") {
        if (!query_text.starts_with("image-source:") ||
            (step.outcome != "pending" && step.outcome != "uncertain") ||
            !text(find(content, "schema")) ||
            find(content, "schema")->as_string() != "rozephine-image-caption-proposal-v1" ||
            !exact_bool(find(content, "raw_pixels_sent"), true) ||
            !exact_bool(find(content, "proposal_only"), true) ||
            !exact_bool(find(content, "semantics_verified"), false) ||
            integer(find(content, "new_independent_observation_count"), "caption") != 0 ||
            !exact_bool(find(content, "same_source_reobservation"), true) ||
            !exact_bool(find(content, "counts_as_growth"), false) ||
            !truthy(find(content, "source_address")) || !truthy(find(content, "model")))
            throw std::invalid_argument(
                "image caption must remain source-bound pixel proposal");
        for (const auto key : {"source_sha256", "wire_image_sha256", "request_sha256",
                               "model_sha256", "projector_sha256"})
            if (!text(find(content, key)) || !is_sha256(find(content, key)->as_string()))
                throw std::invalid_argument("image caption digest provenance incomplete");
        const auto [native_width, native_height] = dimensions(
            "oriented_dimensions", "image caption high-resolution geometry changed");
        const auto [delivered_width, delivered_height] = dimensions(
            "delivered_dimensions", "image caption high-resolution geometry changed");
        const auto native_ratio = static_cast<double>(native_width) / native_height;
        const auto delivered_ratio = static_cast<double>(delivered_width) / delivered_height;
        const auto native_pixels = static_cast<long double>(native_width) * native_height;
        const auto delivered_pixels = static_cast<long double>(delivered_width) * delivered_height;
        if (std::abs(delivered_ratio - native_ratio) >
                .005 * std::max(1.0, native_ratio) || delivered_pixels > 1'005'000 ||
            (native_pixels > 1'000'000 && delivered_pixels < 995'000))
            throw std::invalid_argument(
                "image caption high-resolution geometry changed");
        const auto& proposal = object(find(content, "proposal"),
            "image caption bounded interpretation required");
        const std::set<std::string, std::less<>> expected{
            "visual_summary", "tag_agreement", "uncertainty"};
        if (proposal.size() != expected.size() ||
            std::ranges::any_of(expected, [&](const auto& key) {
                const auto* value = find(proposal, key);
                return !value || !std::holds_alternative<std::string>(value->storage()) ||
                       value->as_string().size() > 4096;
            }) || !text(find(proposal, "visual_summary")))
            throw std::invalid_argument(
                "image caption bounded interpretation required");
        if (const auto* tags = find(content, "tags")) {
            for (const auto& value : array(tags, "caption tags")) {
                const auto& tag = object(&value, "caption tags");
                const auto category = integer(find(tag, "category"), "caption tags");
                const auto score = number(find(tag, "score"), "caption tags");
                if (!truthy(find(tag, "name")) ||
                    (category != 0 && category != 4 && category != 9) ||
                    !std::isfinite(score) || score < 0 || score > 1)
                    throw std::invalid_argument(
                        "image caption tag proposal provenance changed");
            }
        }
    } else if (kind == "image_tag_observation") {
        JsonValue::Array cue_values;
        for (const auto& cue : cues) cue_values.emplace_back(cue);
        JsonValue::Array relation_values;
        for (const auto& relation : step.relations) relation_values.emplace_back(relation);
        JsonValue envelope(JsonValue::Object{
            {"cues", std::move(cue_values)},
            {"step", JsonValue::Object{{"phase", step.phase},
                {"observation", step.observation}, {"relations", std::move(relation_values)},
                {"outcome", step.outcome}}}});
        validate_image_tag_experience(envelope);
    } else if (kind == "audio_waveform") {
        const auto& spectrum = array(find(content, "spectrum_probe_log16"),
            "complete-duration acoustic observation required");
        if (!exact_bool(find(content, "full_duration_decoded"), true) ||
            number(find(content, "frames"), "audio frames") <= 0 ||
            number(find(content, "sample_rate"), "audio rate") <= 0 ||
            spectrum.size() != 16 || !truthy(find(content, "aliases")) ||
            step.outcome != "pending")
            throw std::invalid_argument(
                "complete-duration acoustic observation required");
        for (const auto& value : spectrum)
            if (!std::isfinite(number(&value, "nonfinite acoustic features")))
                throw std::invalid_argument("nonfinite acoustic features");
        if (!std::isfinite(number(find(content, "rms"), "nonfinite acoustic features")) ||
            !std::isfinite(number(find(content, "peak"), "nonfinite acoustic features")))
            throw std::invalid_argument("nonfinite acoustic features");
    } else if (kind == "source_exception") {
        if (!truthy(find(content, "error")) ||
            (step.outcome != "failure" && step.outcome != "uncertain"))
            throw std::invalid_argument(
                "source exception must retain unresolved evidence");
    } else throw std::invalid_argument("unsupported generic media kind");
}

std::vector<std::string> media_publication_queries(
    const std::vector<SemanticSourceEpisode>& episodes) {
    std::vector<std::string> result;
    std::set<std::string, std::less<>> seen;
    for (const auto& episode : episodes) {
        if (episode.steps.empty())
            throw std::invalid_argument("legacy outcome lacks temporal profile cue");
        const auto& observation = episode.steps.front().observation;
        std::string query;
        if (observation.is_object()) {
            const auto* schema = find(observation.as_object(), "schema_version");
            if (text(schema) && schema->as_string() == authored_media_observation_schema) {
                const auto* query_value = find(observation.as_object(), "observation_query");
                if (!text(query_value) ||
                    std::ranges::find(episode.cues, query_value->as_string()) == episode.cues.end())
                    throw std::invalid_argument("media query absent from episode");
                query = query_value->as_string();
            }
        }
        if (query.empty()) {
            const auto profile = std::ranges::find_if(episode.cues, [](const auto& cue) {
                return cue.starts_with("phase7-temporal-profile-v2:");
            });
            if (profile == episode.cues.end())
                throw std::invalid_argument("legacy outcome lacks temporal profile cue");
            query = *profile;
        }
        if (seen.insert(query).second) result.push_back(std::move(query));
    }
    if (result.empty()) throw std::invalid_argument("empty observation wave");
    return result;
}

}  // namespace swegca::world
