#include "world/session_result_collection.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace swegca::world {
namespace {

using StepKey = std::pair<std::string, std::size_t>;
using SourceKey = std::tuple<std::string, std::size_t, std::size_t, std::string>;

[[nodiscard]] std::vector<const BoundSessionOccurrence*> event_occurrences(
    const PreparedSessionEntry& entry, const std::size_t ordinal) {
    std::vector<const BoundSessionOccurrence*> result;
    if (const auto found = entry.occurrence_index.by_event.find(ordinal);
        found != entry.occurrence_index.by_event.end())
        for (const auto index : found->second)
            result.push_back(&entry.occurrence_index.occurrences.at(index));
    return result;
}

[[nodiscard]] const SessionFragment* find_fragment(
    const PreparedSessionEntry& entry, const OccurrenceReference& reference) {
    for (const auto& fragment : entry.document.fragments)
        if (fragment.episode_id == reference.episode_id && fragment.step == reference.step &&
            fragment.variant == reference.variant) return &fragment;
    return nullptr;
}

}  // namespace

SessionResultCollection collect_session_results(
    const PreparedSessionView& view,
    const std::map<std::pair<std::string, std::size_t>, SessionSelectedStep>& selected_steps,
    const bool include_messages, const bool include_event_obligations,
    const bool include_operations) {
    SessionResultCollection output;
    std::map<SessionDocumentKey, PreparedSessionEntryPtr> documents;
    std::map<SessionDocumentKey, std::vector<StepKey>> owners;
    std::set<std::tuple<std::string, std::size_t, std::string, std::string>> unresolved_seen;
    auto retain = [&](const std::string& identity, const std::size_t step,
                            std::string reason,
                            std::optional<SessionEventObligation> obligation = std::nullopt) {
        std::string event_key;
        if (obligation) event_key = obligation->document_key.first + ":" +
            obligation->document_key.second + ":" + std::to_string(obligation->event_ordinal);
        if (unresolved_seen.emplace(identity, step, reason, event_key).second)
            output.unresolved.push_back({identity, step, std::move(reason), std::move(obligation)});
    };

    for (const auto& [selected_key, unused] : selected_steps) {
        (void)unused;
        const auto keys = view.directory.keys_for_step(
            selected_key.first, selected_key.second, view.memory_snapshot_id);
        if (keys.empty()) retain(selected_key.first, selected_key.second,
                                 "session_document_not_indexed");
        for (const auto& key : keys) {
            const auto entry = view.get(key, view.memory_snapshot_id);
            if (!entry) retain(selected_key.first, selected_key.second,
                               "session_document_not_prepared");
            else {
                documents.emplace(key, entry);
                owners[key].push_back(selected_key);
            }
        }
    }
    for (const auto& [unused, entry] : documents) {
        (void)unused;
        output.retained_entries.push_back(entry);
    }

    std::map<SourceKey, SessionAnswerSource> source_cache;
    const auto source_for = [&](const SessionFragment& fragment,
                                const std::string& missing_reason)
        -> const SessionAnswerSource* {
        const auto selected = selected_steps.find({fragment.episode_id, fragment.step});
        if (selected == selected_steps.end()) {
            retain(fragment.episode_id, fragment.step, missing_reason);
            return nullptr;
        }
        if (selected->second.revision != fragment.revision)
            throw std::invalid_argument("session answer source revision changed");
        const SourceKey key{fragment.episode_id, fragment.step, fragment.variant, fragment.revision};
        const auto [found, inserted] = source_cache.emplace(key, SessionAnswerSource{
            fragment.episode_id, fragment.step, fragment.variant, fragment.revision,
            fragment.source_addresses, fragment.outcome, selected->second.current_verdict,
            selected->second.selection_reason});
        (void)inserted;
        return &found->second;
    };

    std::map<SessionCallKey, SessionCallJoin> joins;
    std::map<SessionCallKey, std::vector<RecordedSessionCall>> joined_calls;
    const auto ensure_join = [&](const SessionCallKey& call_key) -> const SessionCallJoin& {
        if (const auto found = joins.find(call_key); found != joins.end()) return found->second;
        auto joined = view.call_join(call_key, view.memory_snapshot_id);
        for (const auto& groups : {&joined.calls, &joined.results})
            for (const auto& group : *groups)
                for (const auto* occurrence : group)
                    for (const auto& reference : occurrence->references) {
                        for (const auto& [unused_key, entry] : documents) {
                            (void)unused_key;
                            if (const auto* fragment = find_fragment(*entry, reference)) {
                                (void)source_for(*fragment,
                                    "session_counterpart_not_activated_and_selected");
                                break;
                            }
                        }
                    }
        std::vector<RecordedSessionCall> calls;
        if (joined.status == "linked_by_scoped_call_id") {
            for (const auto& group : joined.calls) {
                if (group.empty() || !group.front()->event().call_meaning) continue;
                std::map<std::tuple<std::string, std::size_t, std::size_t>, SessionAnswerSource> sources;
                bool all_selected = true;
                std::vector<SessionSourcePosition> positions;
                for (const auto* occurrence : group) {
                    if (std::ranges::find(positions, occurrence->source_position) == positions.end())
                        positions.push_back(occurrence->source_position);
                    for (const auto& reference : occurrence->references) {
                        const SessionFragment* fragment = nullptr;
                        for (const auto& [unused_key, entry] : documents) {
                            (void)unused_key;
                            fragment = find_fragment(*entry, reference);
                            if (fragment) break;
                        }
                        if (!fragment) { all_selected = false; continue; }
                        const auto* source = source_for(
                            *fragment, "session_counterpart_not_activated_and_selected");
                        if (!source) all_selected = false;
                        else sources[{source->episode_id, source->step, source->variant}] = *source;
                    }
                }
                if (all_selected) {
                    std::vector<SessionAnswerSource> selected_sources;
                    for (auto& [unused, source] : sources) {
                        (void)unused; selected_sources.push_back(std::move(source));
                    }
                    calls.push_back({call_key,
                        std::make_shared<const SessionCallMeaning>(*group.front()->event().call_meaning),
                                     std::move(selected_sources), std::move(positions)});
                }
            }
        }
        joined_calls.emplace(call_key, std::move(calls));
        return joins.emplace(call_key, std::move(joined)).first->second;
    };

    for (const auto& [document_key, entry] : documents) {
        if (!entry->document.archive) {
            for (const auto& owner : owners[document_key])
                retain(owner.first, owner.second, "session_document_content_unresolved");
            continue;
        }
        std::vector<SessionAnswerSource> source_rows;
        for (const auto& fragment : entry->document.fragments)
            if (const auto* source = source_for(
                fragment, "session_fragment_not_activated_and_selected"))
                source_rows.push_back(*source);
        if (!entry->occurrence_index.unresolved.empty() || !entry->unresolved_steps.empty())
            for (const auto& source : source_rows)
                retain(source.episode_id, source.step, "session_occurrence_binding_unresolved");

        for (const auto& event : entry->document.archive->events) {
            const auto occurrences = event_occurrences(*entry, event.ordinal);
            const auto retain_event = [&](std::string reason,
                                          std::vector<std::string> details = {},
                                          std::vector<SessionCallLinkStatus> call_links = {}) {
                std::optional<SessionEventObligation> obligation;
                if (include_event_obligations) {
                    std::vector<SessionEventOccurrenceAddress> addresses;
                    for (const auto* occurrence : occurrences)
                        addresses.push_back({occurrence->session, occurrence->turn,
                            occurrence->call_key, occurrence->role, occurrence->source_position});
                    std::vector<std::string> targets;
                    if (event.call_meaning) {
                        for (const auto& target : {event.call_meaning->tool_name,
                                                  event.call_meaning->executable})
                            if (target && std::ranges::find(targets, *target) == targets.end())
                                targets.push_back(*target);
                    }
                    obligation = SessionEventObligation{document_key, event.ordinal,
                        std::move(addresses), std::move(details), std::move(call_links),
                        std::move(targets)};
                }
                for (const auto& source : source_rows)
                    retain(source.episode_id, source.step, reason, obligation);
            };

            if (include_operations && event.operation_meaning) {
                output.operations.push_back({document_key, event.ordinal,
                    std::make_shared<const SessionOperationMeaning>(*event.operation_meaning),
                    source_rows, occurrences});
                if (!event.operation_meaning->unresolved.empty())
                    retain_event("session_operation_content_not_resolved",
                                 event.operation_meaning->unresolved);
                if (occurrences.empty())
                    retain_event("session_operation_occurrence_not_resolved");
                continue;
            }
            if (include_messages && event.message_meaning) {
                output.messages.push_back({document_key, event.ordinal,
                    std::make_shared<const SessionMessageMeaning>(*event.message_meaning),
                    source_rows, occurrences});
                retain_event("session_message_content_semantics_not_resolved",
                             event.message_meaning->unresolved);
                continue;
            }
            if (!event.result_meaning && !event.call_meaning) {
                retain_event("session_event_content_not_resolved");
                continue;
            }
            std::vector<SessionCallKey> call_keys;
            for (const auto* occurrence : occurrences)
                if (occurrence->call_key &&
                    std::ranges::find(call_keys, *occurrence->call_key) == call_keys.end())
                    call_keys.push_back(*occurrence->call_key);
            std::vector<std::string> statuses;
            std::vector<SessionCallLinkStatus> links;
            for (const auto& key : call_keys) {
                const auto& joined = ensure_join(key);
                statuses.push_back(joined.status);
                links.push_back({key, joined.status});
            }
            const auto unlinked = std::ranges::any_of(statuses,
                [](const auto& status) { return status != "linked_by_scoped_call_id"; });
            if (!event.result_meaning) {
                if (!event.call_meaning->unresolved.empty() || call_keys.empty() || unlinked)
                    retain_event("session_call_qualifications_unresolved",
                                 event.call_meaning->unresolved, links);
                continue;
            }
            if (!event.result_meaning->unresolved.empty() || call_keys.empty() || unlinked)
                retain_event("session_result_qualifications_unresolved",
                             event.result_meaning->unresolved, links);
            std::vector<RecordedSessionCall> calls;
            for (const auto& key : call_keys) {
                const auto& rows = joined_calls[key];
                calls.insert(calls.end(), rows.begin(), rows.end());
            }
            bool call_unresolved = calls.empty();
            std::vector<std::string> call_issues;
            for (const auto& call : calls)
                for (const auto& issue : call.meaning->unresolved) {
                    call_unresolved = true;
                    if (std::ranges::find(call_issues, issue) == call_issues.end())
                        call_issues.push_back(issue);
                }
            if (call_unresolved)
                retain_event("session_result_request_content_unresolved",
                             std::move(call_issues), links);
            output.results.push_back({document_key, event.ordinal,
                std::make_shared<const SessionResultMeaning>(*event.result_meaning),
                source_rows, std::move(call_keys), std::move(statuses), std::move(calls)});
        }
    }
    return output;
}

}  // namespace swegca::world
