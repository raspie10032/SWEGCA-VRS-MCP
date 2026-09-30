#include "world/experience_organization.hpp"

#include "world/semantic_vrs_ingress.hpp"
#include "world/vrs_dialogue.hpp"
#include "swegca_architecture/sha256.hpp"

#include <algorithm>
#include <chrono>
#include <future>
#include <set>
#include <span>
#include <stdexcept>
#include <tuple>

namespace swegca::world {
namespace {

void require_text(const std::string_view value, const char* label) {
    if (value.empty()) throw std::invalid_argument(std::string(label) + " must be nonempty text");
}

void require_unique_text(const std::vector<std::string>& values, const char* label) {
    std::set<std::string, std::less<>> unique;
    if (values.empty()) throw std::invalid_argument(std::string(label) + " must be unique and nonempty");
    for (const auto& value : values)
        if (value.empty() || !unique.insert(value).second)
            throw std::invalid_argument(std::string(label) + " must be unique and nonempty");
}

bool digest_id(const std::string_view value) {
    return value.size() == 64 && std::ranges::all_of(value, [](const unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

JsonValue::Array strings(const std::vector<std::string>& values) {
    JsonValue::Array result;
    for (const auto& value : values) result.emplace_back(value);
    return result;
}

std::map<std::string, const OrganizedExperienceItem*, std::less<>> proposal_items(
    const ExperienceOrganizationRequest& request,
    const ExperienceOrganizationProposal& proposal) {
    if (proposal.request_sha256 != request.request_sha256)
        throw std::invalid_argument("experience organization request binding changed");
    if (proposal.base_snapshot_id != request.base_snapshot_id)
        throw std::invalid_argument("experience organization base snapshot binding changed");
    std::map<std::string, const OrganizedExperienceItem*, std::less<>> result;
    for (const auto& item : proposal.items)
        if (!result.emplace(item.source_item_id, &item).second)
            throw std::invalid_argument("experience organization source item partition changed");
    if (result.size() != request.items.size())
        throw std::invalid_argument("experience organization source item partition changed");
    for (const auto& item : request.items) if (!result.contains(item.source_item_id))
        throw std::invalid_argument("experience organization source item partition changed");
    return result;
}

std::vector<std::string> grounded_cues(
    const std::string& source_family, const std::string& task_family,
    const std::string& outcome, const std::vector<std::string>& specialist) {
    std::vector<std::string> result{
        "source-family:" + source_family, "task-family:" + task_family, "outcome:" + outcome};
    for (const auto& cue : specialist) {
        if (cue.starts_with("source-family:") || cue.starts_with("task-family:") ||
            cue.starts_with("outcome:")) continue;
        if (std::ranges::find(result, cue) == result.end()) result.push_back(cue);
        for (auto key : vrs_dialogue_query_keys(cue))
            if (std::ranges::find(result, key) == result.end()) result.push_back(std::move(key));
    }
    return result;
}

std::vector<std::string> grounded_relations(
    const std::string& task_family, const std::string& outcome,
    const std::vector<std::string>& specialist) {
    std::vector<std::string> result{"task-family:" + task_family + "->outcome:" + outcome};
    for (const auto& relation : specialist)
        if (std::ranges::find(result, relation) == result.end()) result.push_back(relation);
    return result;
}

std::optional<std::int64_t> observed_at(const MemoryEpisode& episode) {
    if (episode.steps.empty()) return std::nullopt;
    const auto found = episode.steps.front().observation.find("observed_at_ns");
    if (found == episode.steps.front().observation.end()) return std::nullopt;
    if (const auto* value = std::get_if<std::int64_t>(&found->second.storage()); value && *value >= 0)
        return *value;
    return std::nullopt;
}

std::string rank_digest(const std::string& task, const std::string& candidate) {
    std::string wire = task;
    wire.push_back('\0');
    wire += candidate;
    const auto bytes = architecture::Sha256::of(
        std::as_bytes(std::span(wire.data(), wire.size())));
    constexpr char digits[] = "0123456789abcdef";
    std::string result(bytes.size() * 2, '0');
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        const auto value = std::to_integer<unsigned>(bytes[i]);
        result[i * 2] = digits[value >> 4U];
        result[i * 2 + 1] = digits[value & 15U];
    }
    return result;
}

}  // namespace

JsonValue::Object SealedExperienceItem::canonical_payload() const {
    return {{"source_item_id", source_item_id}, {"observation", observation},
        {"attempted_judgment_or_action", attempted_judgment_or_action}, {"outcome", outcome},
        {"evidence_refs", strings(evidence_refs)}, {"source_addresses", strings(source_addresses)},
        {"source_family", source_family}, {"task_family", task_family},
        {"observed_at_ns", observed_at_ns}, {"revision", revision},
        {"verification_state", verification_state}};
}

ExperienceOrganizationRequest::ExperienceOrganizationRequest(
    std::string snapshot, std::vector<SealedExperienceItem> items_value)
    : base_snapshot_id(std::move(snapshot)), items(std::move(items_value)) {
    require_text(base_snapshot_id, "base snapshot ID");
    if (items.empty()) throw std::invalid_argument("experience organization request must contain items");
    std::set<std::string, std::less<>> ids;
    JsonValue::Array payloads;
    for (const auto& item : items) {
        require_text(item.source_item_id, "source item ID");
        if (item.observation.empty()) throw std::invalid_argument("sealed observation must be a nonempty mapping");
        require_text(item.attempted_judgment_or_action, "attempted judgment or action");
        if (!memory_outcomes.contains(item.outcome))
            throw std::invalid_argument("unsupported sealed experience outcome");
        require_unique_text(item.evidence_refs, "evidence refs");
        require_unique_text(item.source_addresses, "source addresses");
        require_text(item.source_family, "source family");
        require_text(item.task_family, "task family");
        require_text(item.revision, "source revision");
        require_text(item.verification_state, "verification state");
        if (item.observed_at_ns < 0)
            throw std::invalid_argument("observed_at_ns must be a nonnegative integer");
        if (!ids.insert(item.source_item_id).second)
            throw std::invalid_argument("sealed source item IDs must be unique");
        payloads.emplace_back(item.canonical_payload());
    }
    request_sha256 = semantic_json_digest(JsonValue::Object{
        {"schema_version", "rozephine-experience-organization-request-v1"},
        {"base_snapshot_id", base_snapshot_id}, {"items", std::move(payloads)}});
}

ExperienceOrganizationProposal::ExperienceOrganizationProposal(
    std::string specialist, std::string request, std::string base,
    std::vector<OrganizedExperienceItem> items_value)
    : specialist_id(std::move(specialist)), request_sha256(std::move(request)),
      base_snapshot_id(std::move(base)), items(std::move(items_value)) {
    require_text(specialist_id, "specialist ID");
    if (!digest_id(request_sha256))
        throw std::invalid_argument("proposal request SHA-256 changed");
    require_text(base_snapshot_id, "base snapshot ID");
    if (items.empty()) throw std::invalid_argument("experience organization proposal must contain items");
    for (const auto& item : items) {
        require_text(item.source_item_id, "source item ID");
        require_unique_text(item.cues, "organization cues");
        require_unique_text(item.relations, "organization relations");
        require_text(item.judgment, "organized judgment");
    }
}

std::string sealed_experience_episode_id(const SealedExperienceItem& item) {
    return "experience:" + semantic_json_digest(item.canonical_payload());
}

ExperienceOrganizationProposal proposal_from_specialist_output(
    std::string specialist_id, const ExperienceOrganizationRequest& request,
    const JsonValue& payload) {
    if (!payload.is_object() || payload.as_object().size() != 1 ||
        !payload.as_object().contains("items") || !payload.at("items").is_array())
        throw std::invalid_argument("specialist output schema changed");
    std::vector<OrganizedExperienceItem> items;
    for (const auto& value : payload.at("items").as_array()) {
        if (!value.is_object() || value.as_object().size() != 4)
            throw std::invalid_argument("specialist output item schema changed");
        const auto& row = value.as_object();
        if (!row.contains("source_item_id") || !row.contains("cues") ||
            !row.contains("relations") || !row.contains("judgment"))
            throw std::invalid_argument("specialist output item schema changed");
        std::vector<std::string> cues, relations;
        for (const auto& cue : row.at("cues").as_array()) cues.emplace_back(cue.as_string());
        for (const auto& relation : row.at("relations").as_array())
            relations.emplace_back(relation.as_string());
        items.push_back({std::string(row.at("source_item_id").as_string()),
            std::move(cues), std::move(relations), std::string(row.at("judgment").as_string())});
    }
    return ExperienceOrganizationProposal(std::move(specialist_id), request.request_sha256,
        request.base_snapshot_id, std::move(items));
}

ExperienceOrganizationProposal run_experience_organization_specialist(
    const AtomicMemoryActivationOwner& owner, const ExperienceOrganizationRequest& request,
    std::string specialist_id, const ExperienceOrganizationSpecialist& specialist) {
    require_text(specialist_id, "specialist ID");
    auto snapshot = owner.snapshot();
    if (snapshot->snapshot_id() != request.base_snapshot_id)
        throw std::invalid_argument("experience organization base snapshot changed");
    auto proposal = specialist(*snapshot, request);
    if (proposal.specialist_id != specialist_id)
        throw std::invalid_argument("experience organization specialist identity changed");
    (void)proposal_items(request, proposal);
    if (owner.snapshot().get() != snapshot.get())
        throw std::runtime_error("experience organization specialist changed main hot memory");
    return proposal;
}

MainAnalogicalOutcomeJudgment form_main_analogical_outcome_judgment(
    const HotMemoryIndex& index, const MemoryActivationReceipt& activation,
    std::optional<std::string> current_task_family,
    std::optional<std::string> current_candidate_id) {
    if (index.snapshot_id() != activation.snapshot_id)
        throw std::invalid_argument("analogical judgment snapshot changed");
    if (current_task_family.has_value() != current_candidate_id.has_value())
        throw std::invalid_argument("current task family and candidate must be supplied together");
    MainAnalogicalOutcomeJudgment result;
    result.snapshot_id = std::string(index.snapshot_id());
    result.query = activation.recall.query;
    result.current_task_family = std::move(current_task_family);
    result.current_candidate_id = std::move(current_candidate_id);
    std::vector<std::tuple<std::int64_t, std::string, std::string>> current;
    for (const auto& candidate : activation.recall.candidates) {
        const auto& episode = index.episode(candidate.episode_id);
        const bool direct = episode.verification_state == organized_verification_state &&
            episode.steps.size() == 1 && episode.steps.front().phase == "observation_attempt_outcome";
        if (direct && result.current_task_family) {
            const auto& observation = episode.steps.front().observation;
            const auto family = observation.find("task_family");
            const auto candidate_id = observation.find("candidate_id");
            const auto time = observed_at(episode);
            if (family != observation.end() && candidate_id != observation.end() && time &&
                family->second.as_string() == *result.current_task_family &&
                candidate_id->second.as_string() == *result.current_candidate_id)
                current.emplace_back(*time, episode.episode_id, episode.steps.front().outcome);
        }
        if (direct && (episode.steps.front().outcome == "success" ||
                       episode.steps.front().outcome == "failure")) {
            result.selected_episode_ids.push_back(episode.episode_id);
            result.historical_outcomes.push_back(episode.steps.front().outcome);
        } else {
            result.rejected_episode_ids.push_back(episode.episode_id);
            result.rejection_reasons.emplace(episode.episode_id,
                "not a direct organized binary-outcome analogy");
        }
    }
    std::set<std::string, std::less<>> distinct(
        result.historical_outcomes.begin(), result.historical_outcomes.end());
    if (distinct.size() == 1) {
        result.status = "non_authoritative_hypothesis";
        result.hypothesis = *distinct.begin();
    } else if (!distinct.empty()) result.status = "abstain_conflicting_direct_analogies";
    else result.status = "abstain_no_direct_analogy";
    std::ranges::sort(current);
    for (const auto& [unused, id, outcome] : current) {
        static_cast<void>(unused);
        result.current_task_episode_ids.push_back(id);
        result.current_task_outcomes.push_back(outcome);
    }
    if (!current.empty()) {
        result.latest_current_task_episode_id = std::get<1>(current.back());
        result.latest_current_task_outcome = std::get<2>(current.back());
    }
    return result;
}

MainActionCandidateSelection select_main_action_candidate(
    const HotMemoryIndex& index, std::string task_id, std::string query,
    const std::vector<MainActionCandidate>& candidates,
    const std::size_t maximum_parallel_workers,
    const MainCandidateReviewer* resident_candidate_reviewer,
    std::optional<std::string> current_task_family,
    std::vector<std::string> preferred_candidate_ids,
    const bool prefer_least_recent_current_candidate,
    std::map<std::string, std::string, std::less<>> prepared_exploration_ranks) {
    require_text(task_id, "main action task id");
    require_text(query, "main action query");
    if (current_task_family) require_text(*current_task_family, "current task family");
    if (candidates.empty() || !maximum_parallel_workers ||
        (prefer_least_recent_current_candidate && !current_task_family))
        throw std::invalid_argument("main action selection boundary changed");
    std::set<std::string, std::less<>> ids;
    for (const auto& candidate : candidates) {
        require_text(candidate.candidate_id, "main action candidate id");
        if (!ids.insert(candidate.candidate_id).second)
            throw std::invalid_argument("main action candidates must be nonempty and unique");
    }
    std::set<std::string, std::less<>> preferred;
    for (const auto& id : preferred_candidate_ids)
        if (id.empty() || !ids.contains(id) || !preferred.insert(id).second)
            throw std::invalid_argument("preferred main action candidate set changed");
    if (!prepared_exploration_ranks.empty()) {
        if (prepared_exploration_ranks.size() != ids.size())
            throw std::invalid_argument("prepared exploration ranks differ from candidate set");
        for (const auto& id : ids) {
            const auto found = prepared_exploration_ranks.find(id);
            if (found == prepared_exploration_ranks.end() || !digest_id(found->second))
                throw std::invalid_argument("prepared exploration ranks differ from candidate set");
        }
    }
    const auto review = [&](const MainActionCandidate& candidate) {
        const auto started = std::chrono::steady_clock::now();
        auto selection = select_runtime_cues(index,
            query + " candidate=" + candidate.candidate_id, candidate.memory_cues);
        auto activation = activate_memory(index, selection.query, selection.selected_cues,
            [&](const ReplayedEpisode& episode) {
                return CurrentEvidenceVerdict(episode.episode_id,
                    "pre-action:" + task_id + ':' + candidate.candidate_id, "insufficient",
                    "the current bounded action outcome has not been observed", {});
            });
        auto judgment = form_main_analogical_outcome_judgment(index, activation,
            current_task_family,
            current_task_family ? std::optional<std::string>(candidate.candidate_id)
                                : std::nullopt);
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started).count();
        return MainCandidateReviewRow{candidate.candidate_id, std::move(selection), std::move(activation),
            std::move(judgment), elapsed};
    };
    std::vector<MainCandidateReviewRow> reviewed;
    JsonValue::Object resident_review_receipt;
    const auto workers = std::min(maximum_parallel_workers, candidates.size());
    if (resident_candidate_reviewer) {
        auto batch = resident_candidate_reviewer->review_candidates(
            index, task_id, query, candidates, current_task_family);
        reviewed = std::move(batch.rows);
        resident_review_receipt = std::move(batch.receipt);
    } else {
        for (std::size_t start = 0; start < candidates.size(); start += workers) {
            std::vector<std::future<MainCandidateReviewRow>> futures;
            const auto stop = std::min(candidates.size(), start + workers);
            for (std::size_t i = start; i < stop; ++i)
                futures.push_back(std::async(std::launch::async, review, std::cref(candidates[i])));
            for (auto& future : futures) reviewed.push_back(future.get());
        }
    }
    if (reviewed.size() != candidates.size())
        throw std::invalid_argument("resident candidate review set changed");
    MainActionCandidateSelection result;
    result.snapshot_id = std::string(index.snapshot_id());
    result.task_id = task_id;
    result.query = query;
    result.parallel_candidate_review = workers > 1;
    result.maximum_parallel_workers = workers;
    result.current_task_family = current_task_family;
    result.preferred_candidate_ids = preferred_candidate_ids;
    result.prefer_least_recent_current_candidate = prefer_least_recent_current_candidate;
    for (auto& row : reviewed) {
        if (!ids.contains(row.candidate_id) ||
            !result.candidate_review_elapsed_ns.emplace(row.candidate_id, row.elapsed_ns).second)
            throw std::invalid_argument("resident candidate review set changed");
        result.cue_selections.emplace(row.candidate_id, std::move(row.cue_selection));
        result.activations.emplace(row.candidate_id, std::move(row.activation));
        result.candidate_judgments.emplace(row.candidate_id, std::move(row.judgment));
    }
    result.resident_candidate_review_receipt = resident_review_receipt;
    result.candidate_receipts_deferred = !resident_review_receipt.empty();
    result.candidate_review_backend = result.candidate_receipts_deferred
        ? "resident_numeric_deferred_receipt" : "cpu_full_receipt";
    result.resident_flat_scheduler_used = resident_candidate_reviewer != nullptr;
    if (const auto found = resident_review_receipt.find("last_batch_elapsed_ns");
        found != resident_review_receipt.end())
        result.candidate_review_batch_elapsed_ns = found->second.as_int();
    std::vector<std::string> historical_successful, historically_unseen;
    for (const auto& [id, judgment] : result.candidate_judgments) {
        if (judgment.hypothesis == "success") historical_successful.push_back(id);
        if (judgment.status == "abstain_no_direct_analogy") historically_unseen.push_back(id);
    }
    std::vector<std::string> successful, unseen;
    if (!current_task_family) {
        successful = historical_successful;
        unseen = historically_unseen;
    } else {
        for (const auto& id : historical_successful)
            if (result.candidate_judgments.at(id).latest_current_task_outcome == "success")
                successful.push_back(id);
        if (successful.empty()) for (const auto& id : historical_successful)
            if (!result.candidate_judgments.at(id).latest_current_task_outcome)
                successful.push_back(id);
        for (const auto& id : historically_unseen)
            if (!result.candidate_judgments.at(id).latest_current_task_outcome) unseen.push_back(id);
    }
    const auto rank = [&](const std::string& id) {
        const auto found = prepared_exploration_ranks.find(id);
        return found == prepared_exploration_ranks.end() ? rank_digest(task_id, id) : found->second;
    };
    const auto recency = [&](const std::string& id) -> std::int64_t {
        const auto& episode_id = result.candidate_judgments.at(id).latest_current_task_episode_id;
        if (!episode_id) return -1;
        const auto value = observed_at(index.episode(*episode_id));
        if (!value) throw std::invalid_argument("current-task action recency evidence changed");
        return *value;
    };
    const auto filter_preferred = [&](const std::vector<std::string>& values) {
        std::vector<std::string> rows;
        for (const auto& id : values) if (preferred.contains(id)) rows.push_back(id);
        return rows;
    };
    auto preferred_successful = filter_preferred(successful);
    auto preferred_unseen = filter_preferred(unseen);
    std::vector<std::string> preferred_attempted;
    for (const auto& id : preferred_candidate_ids)
        if (result.candidate_judgments.at(id).latest_current_task_outcome)
            preferred_attempted.push_back(id);
    std::vector<std::string> untried;
    for (const auto& candidate : candidates)
        if (!result.candidate_judgments.at(candidate.candidate_id).latest_current_task_outcome &&
            (std::ranges::find(historical_successful, candidate.candidate_id) != historical_successful.end() ||
             std::ranges::find(historically_unseen, candidate.candidate_id) != historically_unseen.end()))
            untried.push_back(candidate.candidate_id);
    const auto by_success = [&](const std::string& left, const std::string& right) {
        const auto lcount = result.candidate_judgments.at(left).selected_episode_ids.size();
        const auto rcount = result.candidate_judgments.at(right).selected_episode_ids.size();
        return lcount != rcount ? lcount > rcount : rank(left) < rank(right);
    };
    const auto choose_min_rank = [&](std::vector<std::string> values) -> std::optional<std::string> {
        if (values.empty()) return std::nullopt;
        return *std::ranges::min_element(values, {}, rank);
    };
    if (!preferred_successful.empty()) {
        result.selected_candidate_id = *std::ranges::min_element(preferred_successful, by_success);
        result.status = "selected_historical_success";
    } else if (!preferred_unseen.empty()) {
        result.selected_candidate_id = choose_min_rank(preferred_unseen);
        result.status = "selected_unseen_exploration";
    } else if (!preferred_attempted.empty()) {
        result.selected_candidate_id = *std::ranges::min_element(preferred_attempted,
            [&](const auto& left, const auto& right) {
                return recency(left) != recency(right) ? recency(left) < recency(right)
                                                       : rank(left) < rank(right);
            });
        result.status = "selected_outcome_bearing_reexploration";
    } else if (prefer_least_recent_current_candidate && !untried.empty()) {
        result.selected_candidate_id = choose_min_rank(untried);
        result.status = std::ranges::find(historical_successful, *result.selected_candidate_id) !=
            historical_successful.end() ? "selected_historical_success" : "selected_unseen_exploration";
    } else if (!successful.empty()) {
        if (prefer_least_recent_current_candidate)
            result.selected_candidate_id = *std::ranges::min_element(successful,
                [&](const auto& left, const auto& right) {
                    if (recency(left) != recency(right)) return recency(left) < recency(right);
                    return by_success(left, right);
                });
        else result.selected_candidate_id = *std::ranges::min_element(successful, by_success);
        result.status = "selected_historical_success";
    } else if (!unseen.empty()) {
        result.selected_candidate_id = choose_min_rank(unseen);
        result.status = "selected_unseen_exploration";
    } else if (prefer_least_recent_current_candidate && current_task_family) {
        std::vector<std::string> attempted;
        for (const auto& candidate : candidates)
            if (result.candidate_judgments.at(candidate.candidate_id).latest_current_task_outcome)
                attempted.push_back(candidate.candidate_id);
        if (!attempted.empty()) result.selected_candidate_id = *std::ranges::min_element(attempted,
            [&](const auto& left, const auto& right) {
                return recency(left) != recency(right) ? recency(left) < recency(right)
                                                       : rank(left) < rank(right);
            });
        result.status = result.selected_candidate_id ? "selected_outcome_bearing_reexploration"
                                                     : "abstain_no_safe_candidate";
    } else result.status = "abstain_no_safe_candidate";
    for (const auto& candidate : candidates)
        if (!result.selected_candidate_id || candidate.candidate_id != *result.selected_candidate_id)
            result.rejected_candidate_ids.push_back(candidate.candidate_id);
    std::ranges::sort(result.rejected_candidate_ids);
    result.exploration_method = !preferred.empty() && prefer_least_recent_current_candidate
        ? "preferred_candidate_then_current_task_least_recent_then_sha256"
        : !preferred.empty() ? "preferred_candidate_then_current_task_latest_exact_then_sha256"
        : prefer_least_recent_current_candidate
            ? "current_task_untried_then_least_recent_outcome_then_sha256"
            : current_task_family ? "current_task_latest_exact_candidate_then_sha256"
                                  : "sha256_task_and_candidate_identity";
    return result;
}

MemoryEpisode ground_organized_experience_episode(const MemoryEpisode& episode) {
    if (episode.verification_state != organized_verification_state || episode.steps.size() != 1)
        throw std::invalid_argument("organized experience grounding input changed");
    const auto& step = episode.steps.front();
    const auto source_family = std::string(step.observation.at("source_family").as_string());
    const auto task_family = std::string(step.observation.at("task_family").as_string());
    return MemoryEpisode(episode.episode_id,
        grounded_cues(source_family, task_family, step.outcome, episode.cues),
        {MemoryStep(step.phase, step.observation,
            grounded_relations(task_family, step.outcome, step.relations), step.judgment,
            step.outcome, step.evidence_refs)}, episode.source_addresses, episode.revision,
        episode.verification_state);
}

std::vector<MemoryEpisode> materialize_organized_experience_episodes(
    const HotMemoryIndex& base, const ExperienceOrganizationRequest& request,
    const ExperienceOrganizationProposal& proposal) {
    if (base.snapshot_id() != request.base_snapshot_id)
        throw std::invalid_argument("experience organization base snapshot changed");
    const auto organized = proposal_items(request, proposal);
    std::set<std::string, std::less<>> ids;
    for (const auto& source : request.items) {
        const auto id = sealed_experience_episode_id(source);
        if (!ids.insert(id).second)
            throw std::invalid_argument("sealed experience items resolve to duplicate episode IDs");
        if (base.contains_episode(id))
            throw std::invalid_argument("organized episode ID already exists");
    }
    std::vector<MemoryEpisode> episodes;
    for (const auto& source : request.items) {
        const auto& item = *organized.at(source.source_item_id);
        JsonValue::Object observation{{"observation", source.observation},
            {"attempted_judgment_or_action", source.attempted_judgment_or_action},
            {"source_item_id", source.source_item_id}, {"source_family", source.source_family},
            {"task_family", source.task_family}, {"observed_at_ns", source.observed_at_ns},
            {"source_verification_state", source.verification_state},
            {"organization_specialist_id", proposal.specialist_id},
            {"organization_request_sha256", request.request_sha256}};
        episodes.emplace_back(sealed_experience_episode_id(source),
            grounded_cues(source.source_family, source.task_family, source.outcome, item.cues),
            std::vector<MemoryStep>{MemoryStep("observation_attempt_outcome", std::move(observation),
                grounded_relations(source.task_family, source.outcome, item.relations), item.judgment,
                source.outcome, source.evidence_refs)}, source.source_addresses, source.revision,
            std::string(organized_verification_state));
    }
    return episodes;
}

MainExperienceAssimilationResult assimilate_organization_proposal(
    AtomicMemoryActivationOwner& owner, const ExperienceOrganizationRequest& request,
    const ExperienceOrganizationProposal& proposal) {
    auto base = owner.snapshot();
    if (base->snapshot_id() != request.base_snapshot_id)
        throw std::invalid_argument("experience organization base snapshot changed");
    auto episodes = materialize_organized_experience_episodes(*base, request, proposal);
    auto replacement = append_memory_activation_index(base, episodes);
    owner.replace(request.base_snapshot_id, replacement);
    MainExperienceAssimilationReceipt receipt;
    receipt.request_sha256 = request.request_sha256;
    receipt.specialist_id = proposal.specialist_id;
    receipt.base_snapshot_id = request.base_snapshot_id;
    receipt.next_snapshot_id = std::string(replacement->snapshot_id());
    receipt.retained_episode_count = base->episode_count();
    receipt.added_episode_count = episodes.size();
    receipt.total_episode_count = replacement->episode_count();
    std::set<std::string, std::less<>> families, tasks, outcomes;
    for (const auto& item : request.items) {
        families.insert(item.source_family); tasks.insert(item.task_family); outcomes.insert(item.outcome);
    }
    receipt.source_families.assign(families.begin(), families.end());
    receipt.task_families.assign(tasks.begin(), tasks.end());
    receipt.added_outcomes.assign(outcomes.begin(), outcomes.end());
    return {std::move(replacement), std::move(receipt)};
}

}  // namespace swegca::world
