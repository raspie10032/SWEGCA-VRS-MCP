#include "world/existing_text_preparation.hpp"

#include "swegca_architecture/sha256.hpp"
#include "world/semantic_source_context.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <span>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace swegca::world {

const std::string adjacent_text_context_prompt = R"(
The first supplied anchor is the current work window in one connected source.
Additional anchors supply original source context, not independent observations.
Adjacent-before and adjacent-after spans continue the same text at its edges;
their outer edges may still be incomplete. Preserve original spacing and scope.
Use them to interpret references and conditions of statements in the work window.
Do not turn proximity into identity, causality, truth, or a relation between names.
Bind any contextual support or qualifier to the supplied context anchors too.
Do not claim the entire document or context is understood from one window.
Keep genuinely unresolved references explicit; do not guess their missing parts.
)";

namespace {

using architecture::Sha256;

[[nodiscard]] const JsonValue* find(const JsonValue::Object& object,
                                    const std::string_view key) noexcept {
    const auto found = object.find(key);
    return found == object.end() ? nullptr : &found->second;
}

[[nodiscard]] bool nonblank(const std::string_view value) noexcept {
    return std::ranges::any_of(value, [](const unsigned char byte) {
        return byte != ' ' && byte != '\t' && byte != '\n' && byte != '\r' &&
               byte != '\f' && byte != '\v';
    });
}

[[nodiscard]] std::size_t utf8_length(const std::string_view text) {
    std::size_t result = 0;
    for (const unsigned char byte : text) if ((byte & 0xc0U) != 0x80U) ++result;
    return result;
}

[[nodiscard]] std::size_t utf8_offset(const std::string_view text,
                                      const std::size_t characters) {
    std::size_t count = 0;
    std::size_t offset = 0;
    while (offset < text.size() && count < characters) {
        ++offset;
        while (offset < text.size() &&
               (static_cast<unsigned char>(text[offset]) & 0xc0U) == 0x80U) ++offset;
        ++count;
    }
    if (count != characters) throw std::invalid_argument("character range outside text");
    return offset;
}

[[nodiscard]] std::string utf8_slice(const std::string_view text,
                                     const std::size_t begin,
                                     const std::size_t end) {
    if (begin > end || end > utf8_length(text))
        throw std::invalid_argument("character range outside text");
    const auto left = utf8_offset(text, begin);
    const auto right = utf8_offset(text, end);
    return std::string(text.substr(left, right - left));
}

[[nodiscard]] std::string hex(const architecture::DigestBytes& digest) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result(digest.size() * 2, '0');
    for (std::size_t index = 0; index != digest.size(); ++index) {
        const auto value = std::to_integer<unsigned>(digest[index]);
        result[index * 2] = digits[value >> 4U];
        result[index * 2 + 1] = digits[value & 15U];
    }
    return result;
}

[[nodiscard]] std::string sha256(const std::string_view text) {
    return hex(Sha256::of(std::as_bytes(std::span(text.data(), text.size()))));
}

[[nodiscard]] const JsonValue& at_path(
    const SemanticSourceEpisode& source, const SemanticAnchor& anchor) {
    if (anchor.step < 0 || static_cast<std::size_t>(anchor.step) >= source.steps.size())
        throw std::invalid_argument("existing_text_delivery_changed");
    const JsonValue* value = &source.steps[static_cast<std::size_t>(anchor.step)].observation;
    for (const auto& element : anchor.path) {
        if (const auto* key = std::get_if<std::string>(&element)) value = &value->at(*key);
        else {
            const auto index = std::get<std::int64_t>(element);
            if (!value->is_array() || index < 0 ||
                static_cast<std::size_t>(index) >= value->as_array().size())
                throw std::invalid_argument("existing_text_delivery_changed");
            value = &value->as_array()[static_cast<std::size_t>(index)];
        }
    }
    return *value;
}

[[nodiscard]] std::string anchor_source_value(
    const SemanticSourceEpisode& source, const SemanticAnchor& anchor) {
    const auto& value = at_path(source, anchor);
    const auto* text = std::get_if<std::string>(&value.storage());
    if (!text || anchor.char_range.size() != 2 || anchor.char_range[0] < 0 ||
        anchor.char_range[1] < anchor.char_range[0])
        throw std::invalid_argument("existing_text_delivery_changed");
    return utf8_slice(*text, static_cast<std::size_t>(anchor.char_range[0]),
                      static_cast<std::size_t>(anchor.char_range[1]));
}

[[nodiscard]] const std::string& text_content(const SemanticDeliveredPart& part) {
    const auto* text = std::get_if<std::string>(&part.content);
    if (!text) throw std::invalid_argument("existing_text_delivery_changed");
    return *text;
}

[[nodiscard]] JsonValue strings(const std::vector<std::string>& values) {
    JsonValue::Array result;
    for (const auto& value : values) result.emplace_back(value);
    return result;
}

[[nodiscard]] JsonValue path_json(const std::vector<SemanticPathElement>& path) {
    JsonValue::Array result;
    for (const auto& element : path) {
        if (const auto* index = std::get_if<std::int64_t>(&element)) result.emplace_back(*index);
        else result.emplace_back(std::get<std::string>(element));
    }
    return result;
}

[[nodiscard]] JsonValue integers(const std::vector<std::int64_t>& values) {
    JsonValue::Array result;
    for (const auto value : values) result.emplace_back(value);
    return result;
}

[[nodiscard]] JsonValue anchor_json(const SemanticAnchor& anchor) {
    return JsonValue::Object{{"identifier", anchor.identifier}, {"step", anchor.step},
        {"path", path_json(anchor.path)}, {"modality", anchor.modality},
        {"role", anchor.role}, {"char_range", integers(anchor.char_range)},
        {"region", integers(anchor.region)}, {"time_ns", integers(anchor.time_ns)}};
}

[[nodiscard]] std::vector<std::int64_t> integer_array(const JsonValue& value) {
    if (!value.is_array()) throw std::invalid_argument("invalid_prepared_text_context");
    std::vector<std::int64_t> result;
    for (const auto& item : value.as_array()) {
        const auto* integer = std::get_if<std::int64_t>(&item.storage());
        if (!integer) throw std::invalid_argument("invalid_prepared_text_context");
        result.push_back(*integer);
    }
    return result;
}

[[nodiscard]] SemanticAnchor parse_anchor(const JsonValue& value) {
    if (!value.is_object()) throw std::invalid_argument("invalid_prepared_text_context");
    const auto& object = value.as_object();
    if (object.size() != 8) throw std::invalid_argument("invalid_prepared_text_context");
    const auto* identifier = std::get_if<std::string>(&value.at("identifier").storage());
    const auto* step = std::get_if<std::int64_t>(&value.at("step").storage());
    const auto* modality = std::get_if<std::string>(&value.at("modality").storage());
    const auto* role = std::get_if<std::string>(&value.at("role").storage());
    if (!identifier || !step || !modality || !role || !value.at("path").is_array())
        throw std::invalid_argument("invalid_prepared_text_context");
    std::vector<SemanticPathElement> path;
    for (const auto& item : value.at("path").as_array()) {
        if (const auto* index = std::get_if<std::int64_t>(&item.storage())) path.emplace_back(*index);
        else if (const auto* key = std::get_if<std::string>(&item.storage())) path.emplace_back(*key);
        else throw std::invalid_argument("invalid_prepared_text_context");
    }
    return {*identifier, *step, std::move(path), *modality, *role,
            integer_array(value.at("char_range")), integer_array(value.at("region")),
            integer_array(value.at("time_ns"))};
}

[[nodiscard]] std::vector<SemanticDeliveredPart> windows(
    const SemanticSourceEpisode& source,
    const std::vector<SemanticDeliveredPart>& parts,
    const std::size_t maximum_characters,
    std::map<std::string, SemanticDeliveredPart, std::less<>>* parents) {
    if (!maximum_characters) throw std::invalid_argument("positive_text_window_required");
    if (parts.empty()) throw std::invalid_argument("unique_text_parts_required");
    std::set<std::string, std::less<>> identifiers;
    std::vector<SemanticDeliveredPart> result;
    for (const auto& part : parts) {
        if (!identifiers.insert(part.anchor.identifier).second)
            throw std::invalid_argument("unique_text_parts_required");
        const auto& content = text_content(part);
        if (part.anchor.modality != "text" || part.mime_type != "text/plain" ||
            content != anchor_source_value(source, part.anchor) ||
            part.anchor.char_range.size() != 2)
            throw std::invalid_argument("existing_text_delivery_changed");
        const auto length = utf8_length(content);
        const auto base = static_cast<std::size_t>(part.anchor.char_range[0]);
        for (std::size_t begin = 0; begin < length; begin += maximum_characters) {
            const auto end = std::min(length, begin + maximum_characters);
            auto anchor = part.anchor;
            anchor.identifier += "@" + std::to_string(base + begin) + ":" +
                                 std::to_string(base + end);
            anchor.char_range = {static_cast<std::int64_t>(base + begin),
                                 static_cast<std::int64_t>(base + end)};
            SemanticDeliveredPart window{anchor, utf8_slice(content, begin, end), "text/plain"};
            if (parents) parents->emplace(anchor.identifier, part);
            result.push_back(std::move(window));
        }
    }
    identifiers.clear();
    for (const auto& part : result)
        if (!identifiers.insert(part.anchor.identifier).second)
            throw std::invalid_argument("unique_text_windows_required");
    return result;
}

[[nodiscard]] std::vector<SemanticDeliveredPart> adjacent_parts(
    const SemanticAnchor& anchor, const SemanticDeliveredPart& parent,
    const std::size_t maximum_characters) {
    const auto begin = static_cast<std::size_t>(anchor.char_range.at(0));
    const auto end = static_cast<std::size_t>(anchor.char_range.at(1));
    const auto low = static_cast<std::size_t>(parent.anchor.char_range.at(0));
    const auto high = static_cast<std::size_t>(parent.anchor.char_range.at(1));
    const auto& content = text_content(parent);
    std::vector<SemanticDeliveredPart> result;
    const std::array rows{
        std::tuple{std::string_view("before"), std::max(low, begin > maximum_characters ? begin - maximum_characters : 0U), begin},
        std::tuple{std::string_view("after"), end, std::min(high, end + maximum_characters)}};
    for (const auto& [side, left, right] : rows) {
        if (left >= right) continue;
        auto neighbor = anchor;
        neighbor.identifier += ":adjacent-" + std::string(side) + "-v1@" +
                               std::to_string(left) + ":" + std::to_string(right);
        neighbor.char_range = {static_cast<std::int64_t>(left),
                               static_cast<std::int64_t>(right)};
        result.push_back({std::move(neighbor),
            utf8_slice(content, left - low, right - low), "text/plain"});
    }
    return result;
}

[[nodiscard]] const SemanticEncoding& only_proposal(const PreparedSemanticBatch& batch) {
    if (batch.proposals.size() != 1)
        throw std::invalid_argument("prepared_text_chunk_proposal_required");
    return batch.proposals.front();
}

[[nodiscard]] JsonValue batch_chunk_receipt(const PreparedSemanticBatch& batch) {
    JsonValue::Array failures;
    for (const auto& failure : batch.failures) failures.emplace_back(failure.receipt());
    return JsonValue::Object{{"proposal", semantic_encoding_receipt(only_proposal(batch))},
                             {"failures", std::move(failures)},
                             {"elapsed_ns", batch.elapsed_ns}};
}

[[nodiscard]] bool string_set_equal(const std::vector<std::string>& left,
                                    const std::vector<std::string>& right) {
    return std::set<std::string, std::less<>>(left.begin(), left.end()) ==
           std::set<std::string, std::less<>>(right.begin(), right.end());
}

}  // namespace

std::vector<SemanticDeliveredPart> archived_document_parts(
    const SemanticSourceEpisode& source) {
    std::vector<SemanticDeliveredPart> parts;
    for (std::size_t step_index = 0; step_index != source.steps.size(); ++step_index) {
        const auto& observation = source.steps[step_index].observation;
        if (!observation.is_object())
            throw SourceAdapterUnavailable("archived_document_schema_unavailable");
        const auto& object = observation.as_object();
        const auto* schema = find(object, "schema_version");
        if (!schema || !std::holds_alternative<std::string>(schema->storage()) ||
            schema->as_string() != existing_text_document_schema)
            throw SourceAdapterUnavailable("archived_document_schema_unavailable");
        std::vector<std::pair<std::vector<SemanticPathElement>, const JsonValue::Object*>> values;
        values.push_back({{}, &object});
        if (const auto* variants = find(object, "rendered_document_variants")) {
            if (!variants->is_array())
                throw SourceAdapterUnavailable("archived_document_variants_unavailable");
            for (std::size_t index = 0; index != variants->as_array().size(); ++index) {
                const auto& row = variants->as_array()[index];
                if (!row.is_object())
                    throw SourceAdapterUnavailable("archived_document_variant_unavailable");
                values.push_back({{std::string("rendered_document_variants"),
                                   static_cast<std::int64_t>(index)}, &row.as_object()});
            }
        }
        std::set<std::string, std::less<>> seen;
        for (auto& [prefix, row] : values) {
            const auto* text_value = find(*row, "normalized_document_text");
            const auto* text = text_value
                ? std::get_if<std::string>(&text_value->storage()) : nullptr;
            if (!text || !nonblank(*text))
                throw SourceAdapterUnavailable("archived_document_text_unavailable");
            const auto* byte_value = find(*row, "normalized_text_bytes");
            const auto* byte_count = byte_value
                ? std::get_if<std::int64_t>(&byte_value->storage()) : nullptr;
            const auto* digest_value = find(*row, "normalized_text_sha256");
            const auto* digest_text = digest_value
                ? std::get_if<std::string>(&digest_value->storage()) : nullptr;
            if (!byte_count || *byte_count != static_cast<std::int64_t>(text->size()) ||
                !digest_text || *digest_text != sha256(*text))
                throw std::invalid_argument("archived_document_text_binding_changed");
            if (!seen.insert(*text).second) continue;
            prefix.emplace_back("normalized_document_text");
            parts.push_back({{"document-" + std::to_string(step_index) + "-" +
                              std::to_string(parts.size()),
                              static_cast<std::int64_t>(step_index), std::move(prefix),
                              "text", "original", {0, static_cast<std::int64_t>(utf8_length(*text))}, {}, {}},
                             *text, "text/plain"});
        }
    }
    if (parts.empty())
        throw SourceAdapterUnavailable("archived_document_text_unavailable");
    return parts;
}

JsonValue PreparedExistingText::receipt() const {
    JsonValue::Array chunks_json;
    for (const auto& chunk : chunks) chunks_json.push_back(batch_chunk_receipt(chunk));
    JsonValue::Object result{{"schema", "rozephine-existing-text-preparation-v1"},
        {"source_ids", strings(batch.source_ids)}, {"chunks", std::move(chunks_json)},
        {"proposal", semantic_encoding_receipt(only_proposal(batch))},
        {"pending_anchors", strings(pending_anchors)},
        {"calls_this_slice", static_cast<std::int64_t>(calls_this_slice)},
        {"window_count", static_cast<std::int64_t>(window_count)},
        {"all_windows_attempted", pending_anchors.empty()},
        {"cross_window_context_independently_evaluated", false},
        {"whole_source_meaning_completed", false}, {"original_reingested", false},
        {"new_observation_count", 0}, {"independent_evidence_count", 0},
        {"main_mutated", false}, {"vrs_recomputed", false}, {"grants_authority", false}};
    if (!context_parts.empty()) {
        JsonValue::Array anchors;
        for (const auto& part : context_parts) anchors.push_back(anchor_json(part.anchor));
        result.emplace("context_anchors", std::move(anchors));
    }
    if (window_context_characters) {
        result.emplace("window_context", JsonValue::Object{
            {"schema", adjacent_text_context_schema},
            {"maximum_characters_each_side", static_cast<std::int64_t>(window_context_characters)},
            {"contextual_windows", strings(contextual_windows)},
            {"adjacency_is_causality", false}});
    }
    return result;
}

std::vector<PreparedSemanticBatch> restore_text_chunks(
    const JsonValue::Array& rows, const SemanticSourceEpisode& source) {
    std::vector<PreparedSemanticBatch> chunks;
    for (const auto& row_value : rows) {
        if (!row_value.is_object() || row_value.as_object().size() != 3 ||
            !row_value.as_object().contains("proposal") ||
            !row_value.as_object().contains("failures") ||
            !row_value.as_object().contains("elapsed_ns"))
            throw std::invalid_argument("invalid_prepared_text_checkpoint");
        const auto* elapsed = std::get_if<std::int64_t>(&row_value.at("elapsed_ns").storage());
        if (!elapsed || *elapsed < 0 || !row_value.at("failures").is_array() ||
            row_value.at("failures").as_array().size() > 1)
            throw std::invalid_argument("invalid_prepared_text_checkpoint");
        auto proposal = restore_semantic_encoding(row_value.at("proposal"), source);
        std::vector<SemanticPreparationFailure> failures;
        for (const auto& failure_value : row_value.at("failures").as_array()) {
            if (!failure_value.is_object())
                throw std::invalid_argument("invalid_prepared_text_failure");
            SemanticPreparationFailure failure =
                SemanticPreparationFailure::from_receipt(failure_value.as_object());
            if (failure.source_id != source.episode_id ||
                failure.source_revision != source.revision || !failure.proposal_created ||
                !string_set_equal(proposal.unresolved, [&] {
                    std::vector<std::string> ids;
                    for (const auto& anchor : proposal.anchors) ids.push_back(anchor.identifier);
                    return ids;
                }()))
                throw std::invalid_argument("invalid_prepared_text_failure");
            if (failure.partial_unit_count) {
                if (!failure.response_utf8)
                    throw std::invalid_argument("invalid_prepared_text_partial_units");
                const auto expected = complete_semantic_unit_prefix(
                    *failure.response_utf8, proposal.anchors);
                if (expected.size() != failure.partial_unit_count || expected != proposal.units)
                    throw std::invalid_argument("invalid_prepared_text_partial_units");
            } else if (!proposal.units.empty())
                throw std::invalid_argument("invalid_prepared_text_failure");
            failures.push_back(std::move(failure));
        }
        chunks.push_back({{source.episode_id}, {std::move(proposal)},
                          std::move(failures), *elapsed});
    }
    return chunks;
}

PreparedExistingText prepare_existing_text(
    const SemanticSourceEpisode& source, const SemanticPartsAdapter& parts_for,
    std::string model, const SemanticProducer& producer,
    const std::size_t maximum_characters, const std::size_t maximum_calls,
    std::vector<PreparedSemanticBatch> completed,
    std::vector<SemanticDeliveredPart> context_parts,
    const std::size_t window_context_characters,
    std::vector<std::string> contextual_windows) {
    if (!nonblank(model)) throw std::invalid_argument("semantic_model_binding_required");
    std::set<std::string, std::less<>> applied(
        contextual_windows.begin(), contextual_windows.end());
    if (applied.size() != contextual_windows.size() ||
        (!applied.empty() && !window_context_characters))
        throw std::invalid_argument("invalid_prepared_window_context_policy");

    std::map<std::string, SemanticDeliveredPart, std::less<>> parents;
    const auto source_parts = parts_for(source);
    auto work = windows(source, source_parts, maximum_characters,
                        window_context_characters ? &parents : nullptr);
    std::map<std::string, SemanticAnchor, std::less<>> expected;
    for (const auto& part : work) expected.emplace(part.anchor.identifier, part.anchor);
    std::map<std::string, SemanticAnchor, std::less<>> contexts;
    for (const auto& part : context_parts) {
        if (part.anchor.modality != "text" || part.mime_type != "text/plain" ||
            text_content(part) != anchor_source_value(source, part.anchor) ||
            expected.contains(part.anchor.identifier) ||
            !contexts.emplace(part.anchor.identifier, part.anchor).second)
            throw std::invalid_argument("prepared_text_context_changed");
    }

    std::map<std::string, PreparedSemanticBatch, std::less<>> accepted;
    std::map<std::string, std::vector<SemanticDeliveredPart>, std::less<>> neighbors;
    const auto context_for = [&](const SemanticAnchor& anchor) -> const std::vector<SemanticDeliveredPart>& {
        auto found = neighbors.find(anchor.identifier);
        if (found != neighbors.end()) return found->second;
        const auto parent = parents.find(anchor.identifier);
        if (parent == parents.end()) throw std::invalid_argument("prepared_text_window_changed");
        auto parts = adjacent_parts(anchor, parent->second, window_context_characters);
        for (const auto& part : parts)
            if (expected.contains(part.anchor.identifier) || contexts.contains(part.anchor.identifier))
                throw std::invalid_argument("prepared_window_context_id_collision");
        return neighbors.emplace(anchor.identifier, std::move(parts)).first->second;
    };

    for (auto& chunk : completed) {
        const auto proposals = chunk.for_wave({source});
        if (proposals.size() != 1)
            throw std::invalid_argument("prepared_text_chunk_proposal_required");
        const auto proposal = restore_semantic_encoding(
            semantic_encoding_receipt(proposals.front()), source);
        if (proposal.anchors.empty())
            throw std::invalid_argument("prepared_text_window_changed");
        const auto& key = proposal.anchors.front().identifier;
        const auto expected_anchor = expected.find(key);
        const auto adjacent = applied.contains(key) && expected_anchor != expected.end()
            ? context_for(expected_anchor->second) : std::vector<SemanticDeliveredPart>{};
        std::map<std::string, SemanticAnchor, std::less<>> allowed = contexts;
        for (const auto& part : adjacent) allowed.emplace(part.anchor.identifier, part.anchor);
        bool valid = proposal.model == model && expected_anchor != expected.end() &&
                     proposal.anchors.front() == expected_anchor->second && !accepted.contains(key);
        std::vector<SemanticAnchor> seen_adjacent;
        for (std::size_t index = 1; valid && index < proposal.anchors.size(); ++index) {
            const auto found = allowed.find(proposal.anchors[index].identifier);
            valid = found != allowed.end() && found->second == proposal.anchors[index];
            if (valid && std::ranges::find_if(adjacent, [&](const auto& part) {
                    return part.anchor.identifier == proposal.anchors[index].identifier;
                }) != adjacent.end()) seen_adjacent.push_back(proposal.anchors[index]);
        }
        std::vector<SemanticAnchor> expected_adjacent;
        for (const auto& part : adjacent) expected_adjacent.push_back(part.anchor);
        if (!valid || seen_adjacent != expected_adjacent)
            throw std::invalid_argument("prepared_text_window_changed");
        accepted.emplace(key, std::move(chunk));
    }
    for (const auto& key : applied)
        if (!accepted.contains(key))
            throw std::invalid_argument("prepared_context_window_not_completed");

    const SemanticProducer contextual_producer = [&](const SemanticEncodingInput& input) {
        auto request = input;
        request.prompt += adjacent_text_context_prompt;
        return producer(request);
    };
    std::size_t calls = 0;
    for (const auto& part : work) {
        const auto& key = part.anchor.identifier;
        if (accepted.contains(key) || calls >= maximum_calls) continue;
        std::vector<SemanticDeliveredPart> supplied{part};
        supplied.insert(supplied.end(), context_parts.begin(), context_parts.end());
        if (window_context_characters) {
            const auto& adjacent = context_for(part.anchor);
            supplied.insert(supplied.end(), adjacent.begin(), adjacent.end());
            applied.insert(key);
        }
        accepted.emplace(key, prepare_semantic_batch(
            {source}, [supplied](const auto&) { return supplied; }, model,
            window_context_characters ? contextual_producer : producer));
        ++calls;
    }

    std::vector<PreparedSemanticBatch> chunks;
    std::vector<std::string> pending;
    std::vector<std::string> unresolved;
    for (const auto& part : work) {
        const auto found = accepted.find(part.anchor.identifier);
        if (found == accepted.end()) {
            pending.push_back(part.anchor.identifier);
            unresolved.push_back(part.anchor.identifier);
        } else {
            chunks.push_back(found->second);
            if (std::ranges::find(only_proposal(found->second).unresolved,
                                  part.anchor.identifier) !=
                only_proposal(found->second).unresolved.end())
                unresolved.push_back(part.anchor.identifier);
        }
    }
    for (const auto& [key, anchor] : contexts) {
        bool delivered = false;
        bool unresolved_context = false;
        for (const auto& chunk : chunks) {
            const auto& proposal = only_proposal(chunk);
            delivered = delivered || std::ranges::find_if(proposal.anchors, [&](const auto& value) {
                return value.identifier == key;
            }) != proposal.anchors.end();
            unresolved_context = unresolved_context ||
                std::ranges::find(proposal.unresolved, key) != proposal.unresolved.end();
        }
        if (!delivered || unresolved_context) unresolved.push_back(key);
        static_cast<void>(anchor);
    }

    std::vector<std::string> applied_order;
    std::vector<SemanticAnchor> adjacent_anchors;
    for (const auto& part : work) if (applied.contains(part.anchor.identifier)) {
        applied_order.push_back(part.anchor.identifier);
        for (const auto& adjacent : context_for(part.anchor)) {
            adjacent_anchors.push_back(adjacent.anchor);
            const auto& proposal = only_proposal(accepted.at(part.anchor.identifier));
            if (std::ranges::find(proposal.unresolved, adjacent.anchor.identifier) !=
                proposal.unresolved.end()) unresolved.push_back(adjacent.anchor.identifier);
        }
    }

    SemanticEncoding encoding;
    encoding.source_id = source.episode_id;
    encoding.source_revision = source.revision;
    encoding.source_digest = semantic_source_digest(source);
    encoding.source_addresses = source.source_addresses;
    for (const auto& step : source.steps) encoding.outcomes.push_back(step.outcome);
    encoding.model = model;
    for (const auto& part : work) encoding.anchors.push_back(part.anchor);
    for (const auto& [key, anchor] : contexts) {
        static_cast<void>(key); encoding.anchors.push_back(anchor);
    }
    encoding.anchors.insert(encoding.anchors.end(), adjacent_anchors.begin(), adjacent_anchors.end());
    encoding.unresolved = std::move(unresolved);
    std::set<std::string, std::less<>> delivered_context_ids;
    std::size_t offset = 0;
    for (const auto& chunk : chunks) {
        const auto& proposal = only_proposal(chunk);
        encoding.units.insert(encoding.units.end(), proposal.units.begin(), proposal.units.end());
        for (const auto index : proposal.partial_response_units)
            encoding.partial_response_units.push_back(offset + index);
        offset += proposal.units.size();
        for (const auto& row : proposal.input_context) {
            if (!row.is_object()) continue;
            const auto* ids = find(row.as_object(), "anchor_ids");
            if (!ids || !ids->is_array()) continue;
            for (const auto& id : ids->as_array())
                if (const auto* text = std::get_if<std::string>(&id.storage()))
                    delivered_context_ids.insert(*text);
        }
    }
    if (!delivered_context_ids.empty()) {
        std::vector<SemanticAnchor> selected;
        for (const auto& anchor : encoding.anchors)
            if (delivered_context_ids.contains(anchor.identifier)) selected.push_back(anchor);
        for (const auto& row : authored_source_context(source, selected))
            encoding.input_context.push_back(row.to_json());
    }

    std::vector<SemanticPreparationFailure> failures;
    std::int64_t elapsed = 0;
    for (const auto& chunk : chunks) {
        failures.insert(failures.end(), chunk.failures.begin(), chunk.failures.end());
        elapsed += chunk.elapsed_ns;
    }
    PreparedSemanticBatch batch{{source.episode_id}, {std::move(encoding)},
                                std::move(failures), elapsed};
    return {std::move(chunks), std::move(pending), std::move(batch), calls,
            work.size(), std::move(context_parts), window_context_characters,
            std::move(applied_order)};
}

PreparedExistingText restore_prepared_text(
    const JsonValue& receipt, const SemanticSourceEpisode& source,
    const SemanticPartsAdapter& parts_for, const std::size_t maximum_characters) {
    if (!receipt.is_object()) throw std::invalid_argument("invalid_prepared_text_receipt");
    const auto& object = receipt.as_object();
    const auto* schema = find(object, "schema");
    const auto* source_ids = find(object, "source_ids");
    const auto* chunks_value = find(object, "chunks");
    const auto* calls_value = find(object, "calls_this_slice");
    const auto* calls = calls_value
        ? std::get_if<std::int64_t>(&calls_value->storage()) : nullptr;
    if (!schema || !std::holds_alternative<std::string>(schema->storage()) ||
        schema->as_string() != "rozephine-existing-text-preparation-v1" ||
        !source_ids || !source_ids->is_array() || source_ids->as_array().size() != 1 ||
        !std::holds_alternative<std::string>(source_ids->as_array()[0].storage()) ||
        source_ids->as_array()[0].as_string() != source.episode_id ||
        !chunks_value || !chunks_value->is_array() || !calls || *calls < 0 ||
        static_cast<std::size_t>(*calls) > chunks_value->as_array().size())
        throw std::invalid_argument("invalid_prepared_text_receipt");

    std::vector<SemanticDeliveredPart> contexts;
    if (const auto* rows = find(object, "context_anchors")) {
        if (!rows->is_array() || rows->as_array().empty())
            throw std::invalid_argument("invalid_prepared_text_context");
        for (const auto& row : rows->as_array()) {
            auto anchor = parse_anchor(row);
            if (anchor.modality != "text")
                throw std::invalid_argument("invalid_prepared_text_context");
            contexts.push_back({anchor, anchor_source_value(source, anchor), "text/plain"});
        }
    }
    auto chunks = restore_text_chunks(chunks_value->as_array(), source);
    const auto* aggregate = find(object, "proposal");
    if (!aggregate || !aggregate->is_object())
        throw std::invalid_argument("invalid_prepared_text_aggregate");
    const auto* model_value = find(aggregate->as_object(), "model");
    const auto* model = model_value
        ? std::get_if<std::string>(&model_value->storage()) : nullptr;
    if (!model) throw std::invalid_argument("invalid_prepared_text_aggregate");

    std::size_t context_characters = 0;
    std::vector<std::string> contextual_windows;
    if (const auto* policy = find(object, "window_context")) {
        if (!policy->is_object() || policy->as_object().size() != 4)
            throw std::invalid_argument("invalid_prepared_window_context_policy");
        const auto* amount = std::get_if<std::int64_t>(
            &policy->at("maximum_characters_each_side").storage());
        const auto* causal = std::get_if<bool>(&policy->at("adjacency_is_causality").storage());
        if (!amount || *amount <= 0 || !causal || *causal ||
            policy->at("schema").as_string() != adjacent_text_context_schema ||
            !policy->at("contextual_windows").is_array())
            throw std::invalid_argument("invalid_prepared_window_context_policy");
        context_characters = static_cast<std::size_t>(*amount);
        for (const auto& item : policy->at("contextual_windows").as_array())
            contextual_windows.emplace_back(item.as_string());
    }
    const SemanticProducer forbidden = [](const auto&) -> SemanticProducerResult {
        throw std::logic_error("prepared text restoration must never invoke a model");
    };
    auto result = prepare_existing_text(
        source, parts_for, *model, forbidden, maximum_characters, 0,
        std::move(chunks), std::move(contexts), context_characters,
        std::move(contextual_windows));
    auto expected = result.receipt().as_object();
    expected["calls_this_slice"] = *calls;
    if (semantic_canonical_json(receipt) != semantic_canonical_json(JsonValue(expected)))
        throw std::invalid_argument("prepared_text_receipt_changed");
    return result;
}

}  // namespace swegca::world
