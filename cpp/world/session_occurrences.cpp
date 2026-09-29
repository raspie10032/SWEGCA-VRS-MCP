#include "world/session_occurrences.hpp"

#include "transport/json.hpp"

#include <algorithm>
#include <memory_resource>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace swegca::world {
namespace {

[[nodiscard]] const JsonValue* field(const JsonValue& value, const std::string_view key) {
    if (!value.is_object()) return nullptr;
    const auto found = value.as_object().find(key);
    return found == value.as_object().end() ? nullptr : &found->second;
}

[[nodiscard]] std::optional<std::string> string_field(
    const JsonValue& value, const std::string_view key) {
    const auto* child = field(value, key);
    if (!child || !std::holds_alternative<std::string>(child->storage())) return std::nullopt;
    return std::get<std::string>(child->storage());
}

[[nodiscard]] std::optional<std::int64_t> integer_field(
    const JsonValue& value, const std::string_view key) {
    const auto* child = field(value, key);
    if (!child || !std::holds_alternative<std::int64_t>(child->storage())) return std::nullopt;
    return std::get<std::int64_t>(child->storage());
}

[[nodiscard]] bool digest_text(const std::string_view value) noexcept {
    return value.size() == 64 && std::ranges::all_of(value, [](const unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

void token(const JsonValue& value, std::string& out) {
    const auto append = [&](const std::string_view name, const std::string_view scalar) {
        out += name; out += ':'; out += std::to_string(scalar.size()); out += ':'; out += scalar; out += ';';
    };
    if (std::holds_alternative<std::nullptr_t>(value.storage())) { out += "none;"; return; }
    if (const auto* boolean = std::get_if<bool>(&value.storage())) {
        out += *boolean ? "bool:1;" : "bool:0;"; return;
    }
    if (const auto* integer = std::get_if<std::int64_t>(&value.storage())) {
        append("int", std::to_string(*integer)); return;
    }
    if (const auto* number = std::get_if<double>(&value.storage())) {
        std::ostringstream stream; stream.precision(17); stream << *number;
        append("float", stream.str()); return;
    }
    if (const auto* text = std::get_if<std::string>(&value.storage())) {
        append("str", *text); return;
    }
    if (const auto* array = std::get_if<JsonValue::Array>(&value.storage())) {
        out += "array["; for (const auto& child : *array) token(child, out); out += "]"; return;
    }
    out += "object{";
    for (const auto& [key, child] : std::get<JsonValue::Object>(value.storage())) {
        append("key", key); token(child, out);
    }
    out += "}";
}

[[nodiscard]] std::string source_token(const JsonValue& value) {
    std::string result;
    token(value, result);
    return result;
}

[[nodiscard]] std::optional<std::pair<std::string, std::string>> role_for(
    const std::string_view type) {
    if (type == "function_call") return std::pair<std::string, std::string>{"function", "call"};
    if (type == "function_call_output") return std::pair<std::string, std::string>{"function", "result"};
    if (type == "custom_tool_call") return std::pair<std::string, std::string>{"custom", "call"};
    if (type == "custom_tool_call_output") return std::pair<std::string, std::string>{"custom", "result"};
    return std::nullopt;
}

struct Pending final {
    std::shared_ptr<const PreparedSessionArchive> archive;
    std::size_t event_ordinal{};
    std::string session;
    std::string turn;
    std::optional<SessionCallKey> call_key;
    std::optional<std::string> role;
    SessionSourcePosition position;
    std::string claim;
    JsonValue metadata;
    std::vector<OccurrenceReference> references;
};

}  // namespace

const SessionEvent& BoundSessionOccurrence::event() const {
    if (!archive || event_ordinal >= archive->events.size())
        throw std::logic_error("session occurrence event binding changed");
    return archive->events[event_ordinal];
}

SessionOccurrenceIndex prepare_session_occurrences(const SessionDocument* document) {
    SessionOccurrenceIndex result;
    if (!document || !document->archive) {
        result.unresolved.push_back({"document_not_prepared", std::nullopt, std::nullopt});
        return result;
    }
    const auto archive = document->archive;
    std::map<std::string, std::vector<const SessionEvent*>, std::less<>> by_digest;
    for (const auto& event : archive->events)
        by_digest[event.declared_content_sha256].push_back(&event);
    std::vector<Pending> prepared;
    using Signature = std::tuple<std::size_t, SessionSourcePosition, std::string>;
    std::map<Signature, std::size_t> seen;
    for (const auto& fragment : document->fragments) {
        const auto* rows = field(fragment.historical_provenance, "occurrences");
        const auto* files = field(fragment.historical_provenance, "files");
        if (!rows || !rows->is_array()) {
            result.unresolved.push_back({"missing_occurrence_list",
                OccurrenceReference{fragment.episode_id, fragment.step, fragment.variant, 0}, std::nullopt});
            continue;
        }
        for (std::size_t ordinal = 0; ordinal != rows->as_array().size(); ++ordinal) {
            const auto& row = rows->as_array()[ordinal];
            const OccurrenceReference reference{fragment.episode_id, fragment.step,
                                                fragment.variant, ordinal};
            if (!row.is_object()) {
                result.unresolved.push_back({"invalid_occurrence", reference, std::nullopt});
                continue;
            }
            const auto digest = string_field(row, "content_sha256");
            const auto candidates = digest ? by_digest.find(*digest) : by_digest.end();
            const auto candidate_count = candidates == by_digest.end() ? 0 : candidates->second.size();
            if (candidate_count != 1 || !candidates->second.front()->content_digest_matches) {
                const auto reason = candidate_count == 0 ? "missing_payload_binding" :
                    candidate_count != 1 ? "ambiguous_payload_binding" : "payload_digest_mismatch";
                result.unresolved.push_back({reason, reference, std::nullopt});
                continue;
            }
            const auto& event = *candidates->second.front();
            const auto file_id = integer_field(row, "file_id");
            const JsonValue* file = nullptr;
            if (files && files->is_object() && file_id) {
                const auto found = files->as_object().find(std::to_string(*file_id));
                if (found != files->as_object().end()) file = &found->second;
            }
            const auto path = file ? string_field(*file, "path") : std::nullopt;
            const auto file_sha = file ? string_field(*file, "sha256") : std::nullopt;
            const auto boundary = file ? integer_field(*file, "byte_boundary") : std::nullopt;
            const auto line = integer_field(row, "line");
            const auto offset = integer_field(row, "offset");
            const auto bytes = integer_field(row, "bytes");
            const auto valid_position = path && !path->empty() && file_sha && digest_text(*file_sha) &&
                boundary && *boundary >= 1 && line && *line >= 1 && offset && *offset >= 0 &&
                bytes && *bytes >= 1 && *offset <= *boundary && *bytes <= *boundary - *offset;
            if (!file || !valid_position) {
                result.unresolved.push_back({"incomplete_file_position", reference, event.ordinal});
                continue;
            }
            const auto session = string_field(row, "session");
            const auto turn = string_field(row, "turn");
            const auto kind = string_field(row, "kind");
            const auto* metadata = field(row, "item_metadata");
            if (!session || session->empty() || *session == "unknown" || !metadata ||
                !metadata->is_object() || !kind) {
                result.unresolved.push_back({"incomplete_occurrence_scope", reference, event.ordinal});
                continue;
            }
            SessionSourcePosition position{*path, *file_sha, static_cast<std::size_t>(*boundary),
                static_cast<std::size_t>(*line), static_cast<std::size_t>(*offset),
                static_cast<std::size_t>(*bytes)};
            auto claim = source_token(row);
            const Signature signature{event.ordinal, position, claim};
            if (const auto duplicate = seen.find(signature); duplicate != seen.end()) {
                prepared[duplicate->second].references.push_back(reference);
                continue;
            }
            std::optional<SessionCallKey> call_key;
            std::optional<std::string> role;
            if (const auto type = event.payload.string_field("type")) {
                if (const auto family_role = role_for(*type)) {
                    role = family_role->second;
                    const auto call_id = string_field(*metadata, "call_id");
                    if (!turn || turn->empty() || !call_id || call_id->empty()) {
                        result.unresolved.push_back({"incomplete_call_scope", reference, event.ordinal});
                    } else if (*kind != "response_item:" + std::string(*type)) {
                        result.unresolved.push_back({"unrecognized_call_envelope", reference, event.ordinal});
                    } else {
                        call_key = SessionCallKey{*session, *turn, family_role->first, *call_id};
                    }
                }
            }
            seen.emplace(signature, prepared.size());
            prepared.push_back({archive, event.ordinal, *session, turn.value_or(""),
                std::move(call_key), std::move(role), std::move(position), std::move(claim),
                *metadata, {reference}});
        }
    }
    result.occurrences.reserve(prepared.size());
    for (auto& row : prepared) {
        const auto index = result.occurrences.size();
        result.occurrences.push_back({std::move(row.archive), row.event_ordinal,
            std::move(row.session), std::move(row.turn), std::move(row.call_key),
            std::move(row.role), std::move(row.position), std::move(row.claim),
            std::move(row.metadata), std::move(row.references)});
        const auto& occurrence = result.occurrences.back();
        result.by_event[occurrence.event_ordinal].push_back(index);
        result.by_position[occurrence.source_position].push_back(index);
        if (occurrence.call_key) result.by_call[*occurrence.call_key].push_back(index);
    }
    for (const auto& event : archive->events)
        if (!result.by_event.contains(event.ordinal))
            result.unresolved.push_back({"event_without_bound_occurrence", std::nullopt, event.ordinal});
    return result;
}

SessionCallJoin join_session_call(
    const SessionCallKey& key,
    const std::vector<const BoundSessionOccurrence*>& occurrences,
    std::string memory_snapshot_id,
    const std::vector<const BoundSessionOccurrence*>& position_witnesses) {
    using Fingerprint = std::tuple<std::optional<std::string>, std::string, std::string>;
    std::map<SessionSourcePosition, Fingerprint> positions;
    std::map<std::string, std::vector<const BoundSessionOccurrence*>, std::less<>> calls, results;
    bool conflict = false;
    for (const auto* occurrence : occurrences) {
        if (!occurrence || !occurrence->call_key || *occurrence->call_key != key)
            throw std::invalid_argument("session call index key changed");
        const Fingerprint fingerprint{occurrence->role,
            occurrence->event().declared_content_sha256, occurrence->source_claim};
        const auto [found, inserted] = positions.emplace(occurrence->source_position, fingerprint);
        if (!inserted && found->second != fingerprint) conflict = true;
        auto& target = occurrence->role == std::optional<std::string>{"call"} ? calls : results;
        target[occurrence->event().declared_content_sha256].push_back(occurrence);
    }
    for (const auto* witness : position_witnesses) {
        if (!witness) continue;
        const Fingerprint fingerprint{witness->role,
            witness->event().declared_content_sha256, witness->source_claim};
        if (const auto found = positions.find(witness->source_position);
            found != positions.end() && found->second != fingerprint) conflict = true;
    }
    std::string status;
    const auto multiple_positions = [](const auto& rows) {
        std::set<SessionSourcePosition> unique;
        for (const auto& [unused, group] : rows) {
            (void)unused;
            for (const auto* item : group) unique.insert(item->source_position);
        }
        return unique.size() > 1;
    };
    if (conflict) status = "conflicting_source_position";
    else if (calls.size() > 1 || results.size() > 1 ||
             multiple_positions(calls) || multiple_positions(results))
        status = "ambiguous_call_id";
    else if (calls.empty() || results.empty()) status = "counterpart_not_prepared_or_unresolved";
    else {
        const auto* call = calls.begin()->second.front();
        const auto* result = results.begin()->second.front();
        const auto& left = call->source_position;
        const auto& right = result->source_position;
        const bool same_file = std::tie(left.path, left.sha256, left.byte_boundary) ==
                               std::tie(right.path, right.sha256, right.byte_boundary);
        status = same_file && (left.line >= right.line || left.offset + left.bytes > right.offset)
            ? "inconsistent_source_order" : "linked_by_scoped_call_id";
    }
    SessionCallJoin joined{std::move(memory_snapshot_id), key, {}, {}, std::move(status)};
    for (auto& [unused, rows] : calls) { (void)unused; joined.calls.push_back(std::move(rows)); }
    for (auto& [unused, rows] : results) { (void)unused; joined.results.push_back(std::move(rows)); }
    return joined;
}

}  // namespace swegca::world
