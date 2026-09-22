#include "observation.hpp"
#include "unicode.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace swegca::vrs {
namespace {

// SWEGCA: src/swegca_vrs2/store.py@7536139:82-87
std::pair<std::size_t, bool> text_measure(std::string_view bytes) {
    const auto points = decode_utf8(bytes);
    const auto nonblank = std::any_of(points.begin(), points.end(), [](auto point) {
        return !python_space(point);
    });
    return {points.size(), nonblank};
}

// SWEGCA: src/swegca_vrs2/store.py@7536139:82-87
std::string text_field(const Json& value, std::string_view name, std::size_t maximum) {
    const auto* text = std::get_if<std::string>(&value.data);
    if (!text) throw std::runtime_error("invalid_" + std::string(name));
    // An invalid UTF-8 sequence remains distinct from the author's
    // invalid_<field> text/length error, as UnicodeEncodeError does there.
    const auto [length, nonblank] = text_measure(*text);
    if (!nonblank || length > maximum)
        throw std::runtime_error("invalid_" + std::string(name));
    return *text;
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_memory_activation.py@7536139:9
bool accepted_outcome(std::string_view value) {
    constexpr std::array<std::string_view, 6> outcomes{
        "success", "failure", "negative", "uncertain", "conflict", "pending"};
    return std::find(outcomes.begin(), outcomes.end(), value) != outcomes.end();
}

}  // namespace

// SWEGCA: src/swegca_vrs2/store.py@7536139:90-118
Json observation(const Json& arguments) {
    const auto* input = std::get_if<Json::Object>(&arguments.data);
    if (!input) throw std::runtime_error("invalid_observation_fields");
    for (const auto* required : {"request_id", "text", "source", "revision"}) {
        if (!input->contains(required)) throw std::runtime_error("invalid_observation_fields");
    }
    for (const auto& [name, ignored] : *input) {
        (void)ignored;
        if (name != "request_id" && name != "text" && name != "source" &&
            name != "revision" && name != "outcome" && name != "cues" &&
            name != "proposition" && name != "polarity" &&
            name != "supersedes" && name != "metadata")
            throw std::runtime_error("invalid_observation_fields");
    }
    Json::Object row;
    row.emplace("request_id", Json(text_field(arguments.at("request_id"), "request_id", 128)));
    row.emplace("text", Json(text_field(arguments.at("text"), "text", 65536)));
    row.emplace("source", Json(text_field(arguments.at("source"), "source", 1024)));
    row.emplace("revision", Json(text_field(arguments.at("revision"), "revision", 128)));

    const auto outcome = arguments.contains("outcome") ? arguments.at("outcome") : Json(std::string("pending"));
    const auto* outcome_text = std::get_if<std::string>(&outcome.data);
    if (!outcome_text || !accepted_outcome(*outcome_text))
        throw std::runtime_error("invalid_outcome");
    row.emplace("outcome", outcome);

    const auto cues = arguments.contains("cues") ? arguments.at("cues") : Json(Json::Array{});
    const auto* cue_array = std::get_if<Json::Array>(&cues.data);
    if (!cue_array || cue_array->size() > 128) throw std::runtime_error("invalid_cues");
    Json::Array valid_cues;
    valid_cues.reserve(cue_array->size());
    for (const auto& cue : *cue_array)
        valid_cues.emplace_back(text_field(cue, "cue", 128));
    row.emplace("cues", Json(std::move(valid_cues)));

    const auto proposition = arguments.contains("proposition") ? arguments.at("proposition") : Json();
    const auto polarity = arguments.contains("polarity") ? arguments.at("polarity") : Json();
    const bool has_proposition = !std::holds_alternative<std::nullptr_t>(proposition.data);
    const bool has_polarity = !std::holds_alternative<std::nullptr_t>(polarity.data);
    if (has_proposition != has_polarity)
        throw std::runtime_error("proposition_and_polarity_required_together");
    if (has_proposition) {
        text_field(proposition, "proposition", 512);
        const auto* polarity_text = std::get_if<std::string>(&polarity.data);
        if (!polarity_text || (*polarity_text != "support" && *polarity_text != "refute"))
            throw std::runtime_error("invalid_polarity");
    }
    row.emplace("proposition", proposition);
    row.emplace("polarity", polarity);

    const auto supersedes = arguments.contains("supersedes") ? arguments.at("supersedes") : Json();
    if (!std::holds_alternative<std::nullptr_t>(supersedes.data))
        text_field(supersedes, "supersedes", 128);
    row.emplace("supersedes", supersedes);

    const auto metadata = arguments.contains("metadata") ? arguments.at("metadata") : Json(Json::Object{});
    if (!std::holds_alternative<Json::Object>(metadata.data))
        throw std::runtime_error("invalid_metadata");
    const auto normalized = metadata.canonical();
    if (normalized.size() > 16384) throw std::runtime_error("invalid_metadata");
    row.emplace("metadata", Json::parse(normalized));
    return Json(std::move(row));
}

}  // namespace swegca::vrs
