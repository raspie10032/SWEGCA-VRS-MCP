#include "world/session_speech_segments.hpp"

#include "world/unicode_nfkc.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

namespace swegca::world {
namespace {

struct Span final {
    std::size_t byte_begin{}, byte_end{}, char_begin{}, char_end{};
};

[[nodiscard]] std::size_t unicode_length(const std::string_view value) {
    std::size_t count = 0;
    for (std::size_t at = 0; at != value.size(); ++count) {
        const auto first = static_cast<unsigned char>(value[at]);
        const auto size = first < 0x80U ? 1U : (first & 0xe0U) == 0xc0U ? 2U :
            (first & 0xf0U) == 0xe0U ? 3U : (first & 0xf8U) == 0xf0U ? 4U : 0U;
        if (!size || size > value.size() - at) throw std::invalid_argument("invalid session segment UTF-8");
        at += size;
    }
    return count;
}

[[nodiscard]] Span locate(const std::string& text, const std::string& quote,
                          const std::size_t occurrence) {
    if (strip_unicode_whitespace(quote).empty())
        throw std::invalid_argument("session_segment_quote_required");
    if (occurrence >= unicode_length(text))
        throw std::invalid_argument("session_segment_occurrence_missing");
    auto begin = std::string::npos;
    std::size_t search = 0;
    for (std::size_t index = 0; index <= occurrence; ++index) {
        begin = text.find(quote, search);
        if (begin == std::string::npos)
            throw std::invalid_argument("session_segment_quote_not_in_source");
        search = begin + 1;
    }
    return {begin, begin + quote.size(), unicode_length(std::string_view(text).substr(0, begin)),
            unicode_length(std::string_view(text).substr(0, begin + quote.size()))};
}

[[nodiscard]] JsonValue string_array(const std::vector<std::string>& values) {
    JsonValue::Array result;
    for (const auto& value : values) result.emplace_back(value);
    return JsonValue(std::move(result));
}

[[nodiscard]] JsonValue entities(JsonValue::Array values) {
    return JsonValue(std::move(values));
}

[[nodiscard]] JsonValue qualifier_value(const SemanticQualifier& qualifier) {
    return JsonValue::Object{{"kind", qualifier.kind}, {"value", qualifier.value},
                             {"anchors", string_array(qualifier.anchors)}};
}

}  // namespace

SessionSpeechInterpretation from_segment_annotations(
    SessionSpeechInput prepared,
    std::vector<SessionSegmentAnnotation> segments,
    std::vector<std::string> unresolved) {
    std::map<std::string, std::size_t, std::less<>> originals;
    for (std::size_t index = 0; index != prepared.request.parts.size(); ++index)
        originals.emplace(prepared.request.parts[index].anchor.identifier, index);
    for (const auto& identifier : unresolved)
        if (!originals.contains(identifier))
            throw std::invalid_argument("session_segment_unresolved_anchor_changed");
    std::set<std::string> unresolved_set(unresolved.begin(), unresolved.end());
    std::map<std::string, std::vector<std::pair<std::size_t, std::size_t>>, std::less<>> ranges;
    std::vector<std::pair<std::string, std::string>> span_parents;
    std::set<std::string, std::less<>> span_identifiers;
    std::vector<SemanticMeaningUnit> units;
    for (auto& row : segments) {
        const auto parent = originals.find(row.anchor);
        if (parent == originals.end())
            throw std::invalid_argument("session_segment_anchor_changed");
        if (row.anchors.empty() || row.anchors.front() != row.anchor)
            throw std::invalid_argument("session_segment_references_changed");
        for (const auto& identifier : row.anchors)
            if (!originals.contains(identifier))
                throw std::invalid_argument("session_segment_references_changed");
        const auto original = prepared.request.parts[parent->second];
        const auto span = locate(original.content, row.quote, row.occurrence);
        const auto identifier = row.anchor + ":span:" + std::to_string(span.char_begin) +
                                ":" + std::to_string(span.char_end);
        if (!span_identifiers.insert(identifier).second)
            throw std::invalid_argument("session_segment_duplicate_span");
        auto anchor = original.anchor;
        anchor.identifier = identifier;
        anchor.char_range = {static_cast<std::int64_t>(span.char_begin),
                             static_cast<std::int64_t>(span.char_end)};
        prepared.request.parts.push_back({anchor,
            original.content.substr(span.byte_begin, span.byte_end - span.byte_begin),
            original.media_type});
        span_parents.emplace_back(identifier, row.anchor);
        ranges[row.anchor].push_back({span.byte_begin, span.byte_end});
        JsonValue::Object speech{{"schema", "rozephine-speech-value-v1"},
            {"kind", row.kind}, {"speaker", row.speaker},
            {"addressees", entities(row.addressees)},
            {"topics", entities(row.topics)},
            {"content", prepared.request.parts.back().content},
            {"content_anchors", string_array({identifier})}};
        std::vector<std::string> anchors{identifier};
        anchors.insert(anchors.end(), row.anchors.begin(), row.anchors.end());
        units.push_back({"utterance:" + identifier, "utterance", JsonValue(std::move(speech)),
            "affirmed", "reported", std::move(anchors), row.qualifiers, "literal"});
    }
    for (const auto& [identifier, part_index] : originals) {
        const auto& part = prepared.request.parts[part_index];
        std::size_t cursor = 0;
        bool missing = false;
        auto rows = ranges[identifier];
        std::ranges::sort(rows);
        for (const auto [begin, end] : rows) {
            if (begin > cursor &&
                !strip_unicode_whitespace(std::string_view(part.content).substr(cursor, begin - cursor)).empty())
                missing = true;
            cursor = std::max(cursor, end);
        }
        if (!strip_unicode_whitespace(std::string_view(part.content).substr(cursor)).empty())
            missing = true;
        if (missing && !unresolved_set.contains(identifier))
            throw std::invalid_argument("session_segment_uncovered_source_not_unresolved");
    }
    for (auto& context : prepared.request.source_context) {
        const auto original_ids = context.anchor_ids;
        for (const auto& [span, parent] : span_parents)
            if (std::ranges::find(original_ids, parent) != original_ids.end())
                context.anchor_ids.push_back(span);
    }
    auto result = interpret_session_speech(std::move(prepared), std::move(units), std::move(unresolved));
    JsonValue::Array segment_rows;
    for (const auto& row : segments) {
        JsonValue::Array qualifiers;
        for (const auto& qualifier : row.qualifiers) qualifiers.push_back(qualifier_value(qualifier));
        segment_rows.emplace_back(JsonValue::Object{{"anchor", row.anchor}, {"quote", row.quote},
            {"occurrence", static_cast<std::int64_t>(row.occurrence)}, {"kind", row.kind},
            {"speaker", row.speaker}, {"addressees", entities(row.addressees)},
            {"topics", entities(row.topics)}, {"anchors", string_array(row.anchors)},
            {"qualifiers", JsonValue(std::move(qualifiers))}});
    }
    result.segments = JsonValue::Object{{"segments", JsonValue(std::move(segment_rows))},
                                       {"unresolved", string_array(result.unresolved)}};
    return result;
}

}  // namespace swegca::world
