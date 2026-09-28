#include "world/re_evidence_receipt.hpp"

#include <functional>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace swegca::world;

int checks = 0;
#define CHECK(condition) do { ++checks; if (!(condition)) throw std::runtime_error(#condition); } while (false)

template <typename F>
void invalid(F&& function) {
    ++checks;
    try {
        std::invoke(std::forward<F>(function));
    } catch (const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error("expected invalid_argument at check " +
                             std::to_string(checks));
}

CurrentEvidenceDisposition current(std::string ref = "natural:1") {
    return CurrentEvidenceDisposition{
        std::move(ref), "current observation", "natural_observation",
        "sha256:source", "revision:1", "filesystem", "context:1",
        "observational", "support", 1, std::nullopt, "adapter:1", 1.0,
        true, "present"};
}

CandidateDisposition candidate(std::string id = "episode:1", bool selected = true) {
    return CandidateDisposition{
        std::move(id), selected, {"memory:evidence"}, {"sha256:source"},
        "present", "historical_only", "revision:1", "present", std::nullopt};
}

MainReEvidenceReceipt receipt(
    std::string schema = "rozephine-main-re-evidence-receipt-v2",
    std::string verdict = "support", std::optional<std::string> selected = "episode:1",
    std::vector<CandidateDisposition> candidates = {candidate()},
    std::vector<std::string> refs = {"natural:1"},
    std::vector<CurrentEvidenceDisposition> evidence = {current()},
    std::int64_t persistent_count = 1, bool persistent_mutated = false,
    bool manager_retained = false, bool worker_retained = false,
    std::vector<bool> forbidden_authority = {}) {
    const bool conflict = verdict == "conflict";
    const bool insufficient = verdict == "insufficient";
    const bool abstain = conflict || insufficient;
    const bool judgment = verdict == "support" || verdict == "refute";
    forbidden_authority.resize(7, false);
    return MainReEvidenceReceipt(
        std::move(schema), "sole-main", "request-hash", "query",
        std::move(selected), judgment ? std::optional<std::string>("alpha supports beta")
                                     : std::nullopt,
        std::move(verdict), "rationale", std::move(refs), std::move(evidence),
        {"memory:evidence"}, std::move(candidates),
        {ProposalDisposition{"core", "model-output:1", true, std::nullopt}},
        {"core"}, false, 12, conflict, insufficient, abstain, judgment,
        persistent_count, persistent_mutated, manager_retained, worker_retained,
        forbidden_authority[0], forbidden_authority[1], forbidden_authority[2],
        forbidden_authority[3], forbidden_authority[4], forbidden_authority[5],
        forbidden_authority[6]);
}

void check_valid_receipts_and_unvalidated_dispositions() {
    const auto support = receipt();
    CHECK(support.verdict == "support");
    CHECK(support.semantic_judgment_formed);
    CHECK(!support.should_abstain);
    const auto insufficient = receipt(
        "rozephine-main-re-evidence-receipt-v2", "insufficient", std::nullopt,
        {candidate("episode:1", false)});
    CHECK(insufficient.should_abstain);
    CHECK(!insufficient.semantic_judgment_formed);

    // The three disposition dataclasses have no __post_init__ in the source.
    const ProposalDisposition blank{"", "", false, std::nullopt};
    const CandidateDisposition odd{"", false, {}, {}, "", "", std::nullopt, "", std::nullopt};
    const CurrentEvidenceDisposition incomplete{
        "", "", "", std::nullopt, std::nullopt, std::nullopt, std::nullopt,
        std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt,
        std::nullopt, false, ""};
    CHECK(blank.source.empty());
    CHECK(odd.episode_id.empty());
    CHECK(!incomplete.transaction_ready);
}

void check_receipt_invariants() {
    invalid([] { static_cast<void>(receipt("wrong")); });
    invalid([] { static_cast<void>(receipt(
        "rozephine-main-re-evidence-receipt-v2", "other")); });
    invalid([] {
        static_cast<void>(MainReEvidenceReceipt(
            "rozephine-main-re-evidence-receipt-v2", "owner", "hash", "query",
            "episode:1", "claim", "support", "why", {"natural:1"}, {current()},
            {}, {candidate()}, {}, {}, false, 0,
            true, false, false, true));
    });
    invalid([] {
        static_cast<void>(MainReEvidenceReceipt(
            "rozephine-main-re-evidence-receipt-v2", "owner", "hash", "query",
            "episode:1", "claim", "support", "why", {"natural:1"}, {current()},
            {}, {candidate()}, {}, {}, false, 0,
            false, true, false, true));
    });
    invalid([] {
        static_cast<void>(MainReEvidenceReceipt(
            "rozephine-main-re-evidence-receipt-v2", "owner", "hash", "query",
            "episode:1", "claim", "support", "why", {"natural:1"}, {current()},
            {}, {candidate()}, {}, {}, false, 0,
            false, false, true, true));
    });
    invalid([] {
        static_cast<void>(MainReEvidenceReceipt(
            "rozephine-main-re-evidence-receipt-v2", "owner", "hash", "query",
            "episode:1", "claim", "support", "why", {"natural:1"}, {current()},
            {}, {candidate()}, {}, {}, false, 0,
            false, false, false, false));
    });
    invalid([] { static_cast<void>(receipt(
        "rozephine-main-re-evidence-receipt-v2", "support", "episode:1",
        {candidate()}, {"different"}, {current()})); });
    invalid([] { static_cast<void>(receipt(
        "rozephine-main-re-evidence-receipt-v2", "support", "episode:1",
        {candidate(), candidate()})); });
    invalid([] { static_cast<void>(receipt(
        "rozephine-main-re-evidence-receipt-v2", "support", "episode:1",
        {candidate("episode:1", false)})); });
    invalid([] { static_cast<void>(receipt(
        "rozephine-main-re-evidence-receipt-v2", "support", "episode:1",
        {candidate()}, {"natural:1"}, {current()}, 2)); });
    invalid([] { static_cast<void>(receipt(
        "rozephine-main-re-evidence-receipt-v2", "support", "episode:1",
        {candidate()}, {"natural:1"}, {current()}, 1, true)); });
    invalid([] { static_cast<void>(receipt(
        "rozephine-main-re-evidence-receipt-v2", "support", "episode:1",
        {candidate()}, {"natural:1"}, {current()}, 1, false, true)); });
    invalid([] { static_cast<void>(receipt(
        "rozephine-main-re-evidence-receipt-v2", "support", "episode:1",
        {candidate()}, {"natural:1"}, {current()}, 1, false, false, true)); });
    invalid([] { static_cast<void>(receipt(
        "rozephine-main-re-evidence-receipt-v2", "support", "episode:1",
        {candidate()}, {"natural:1"}, {current()}, 1, false, false, false,
        {true})); });
    for (std::size_t index = 0; index != 7; ++index) {
        invalid([index] {
            std::vector<bool> authority(7, false);
            authority[index] = true;
            static_cast<void>(receipt(
                "rozephine-main-re-evidence-receipt-v2", "support", "episode:1",
                {candidate()}, {"natural:1"}, {current()}, 1, false, false,
                false, std::move(authority)));
        });
    }

    const auto refute = receipt(
        "rozephine-main-re-evidence-receipt-v2", "refute", "episode:1",
        {candidate()});
    CHECK(refute.semantic_judgment_formed);
    const auto conflict = receipt(
        "rozephine-main-re-evidence-receipt-v2", "conflict", std::nullopt,
        {candidate("episode:1", false)});
    CHECK(conflict.unresolved_conflict);
    // The receipt constructor only checks the selected partition. It does not
    // add the proposal-layer rule that support/refute must select an episode.
    const auto support_without_selection = receipt(
        "rozephine-main-re-evidence-receipt-v2", "support", std::nullopt,
        {candidate("episode:1", false)});
    CHECK(!support_without_selection.selected_episode_id.has_value());
    invalid([] { static_cast<void>(receipt(
        "rozephine-main-re-evidence-receipt-v2", "support", "episode:2",
        {candidate("episode:1", true), candidate("episode:2", true)})); });
    invalid([] { static_cast<void>(receipt(
        "rozephine-main-re-evidence-receipt-v2", "support", "episode:1",
        {candidate()}, {"natural:2", "natural:1"},
        {current("natural:1"), current("natural:2")})); });
}
}  // namespace

int main() {
    check_valid_receipts_and_unvalidated_dispositions();
    check_receipt_invariants();
    std::cout << "re-evidence receipt tests passed: " << checks << " checks\n";
}
