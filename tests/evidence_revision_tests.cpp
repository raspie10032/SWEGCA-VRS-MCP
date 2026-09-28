#include "world/evidence_revision.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

using namespace swegca::world;

void check(const bool condition, const char* expression, const int line) {
    if (!condition) {
        std::cerr << "CHECK failed at line " << line << ": " << expression << '\n';
        std::exit(1);
    }
}

#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

void write(const std::filesystem::path& path, const std::string& body) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("test artifact open failed");
    output.write(body.data(), static_cast<std::streamsize>(body.size()));
    if (!output) throw std::runtime_error("test artifact write failed");
}

struct Artifacts final {
    std::filesystem::path root;
    std::filesystem::path report;
    std::filesystem::path ledger;
    std::filesystem::path state;
    EvidenceRevisionContract contract;

    explicit Artifacts(const std::filesystem::path& base)
        : root(base), report(root / "report.json"), ledger(root / "ledger.jsonl"),
          state(root / "state.json"), contract(build()) {}

    EvidenceRevisionContract build() {
        std::filesystem::create_directories(root);
        write(state, "{\"state\": 1}\n");
        const auto state_hash = sha256_file(state);
        write(ledger,
            "{\"hypothesis_id\": \"hypothesis:test\", \"update_applied\": true, "
            "\"update_reason\": \"applied\", \"world_hash_before\": \"" + state_hash +
            "\", \"world_hash_after\": \"" + state_hash +
            "\", \"revision\": 1, \"decision\": \"abstain\", "
            "\"posterior_after\": 0.5, \"causal_lower_bound\": 0.0, "
            "\"evidence_addresses\": [\"evidence:1\"]}\n"
            "{\"hypothesis_id\": \"hypothesis:test\", \"update_applied\": true, "
            "\"update_reason\": \"applied\", \"world_hash_before\": \"" + state_hash +
            "\", \"world_hash_after\": \"" + state_hash +
            "\", \"revision\": 2, \"decision\": \"accept\", "
            "\"posterior_after\": 0.75, \"causal_lower_bound\": 0.6, "
            "\"evidence_addresses\": [\"evidence:2\"]}\n");
        write(report,
            "{\"passed\": true, \"accumulator\": {\"status\": \"accept\", "
            "\"revision\": 2, \"posterior_mean\": 0.75, "
            "\"causal_lower_bound\": 0.6}, \"lineage\": {\"state_sha256\": \"" +
            state_hash + "\"}}");
        return EvidenceRevisionContract(
            sha256_file(report), sha256_file(ledger), state_hash, 2,
            "hypothesis:test");
    }

    ~Artifacts() { std::filesystem::remove_all(root); }
};

bool check_value(const EvidenceRevisionVerification& verification,
                 const std::string& name) {
    for (const auto& [key, value] : verification.checks)
        if (key == name) return value;
    throw std::runtime_error("missing verification check");
}

void check_pinned_artifacts(const Artifacts& artifacts) {
    const auto verification = verify_evidence_revision(
        artifacts.report, artifacts.ledger, artifacts.state, artifacts.contract);
    CHECK(verification.evidence_current);
    CHECK(verification.accumulator_revision_current);
    CHECK(verification.checks.size() == 11);
    for (const auto& item : verification.checks) CHECK(item.second);
    CHECK(verification.observed_revision == 2);
    CHECK(verification.evidence_addresses == std::vector<std::string>({
        "sha256:" + artifacts.contract.report_sha256,
        "sha256:" + artifacts.contract.ledger_sha256,
        "sha256:" + artifacts.contract.state_sha256}));
    CHECK(verification.hypothesis_id == artifacts.contract.hypothesis_id);
    CHECK(verification.accepted_evidence_addresses ==
          std::vector<std::string>({"evidence:1", "evidence:2"}));
    CHECK(verification.decision_payload.size() == 12);
    CHECK(verification.decision_payload[0].first == "status");
    CHECK(std::get<std::string>(verification.decision_payload[0].second) == "accept");
    CHECK(verification.decision_payload[1].first == "reason");
    CHECK(std::holds_alternative<std::nullptr_t>(verification.decision_payload[1].second));
    CHECK(verification.decision_payload[10].first == "hypothesis_id");
    CHECK(verification.decision_payload[11].first == "evidence_addresses");
    CHECK(is_authoritative_evidence_revision(verification));

    const EvidenceRevisionVerification copied(verification);
    CHECK(is_authoritative_evidence_revision(copied));
    const EvidenceRevisionVerification foreign(
        verification.evidence_current, verification.accumulator_revision_current,
        verification.checks, verification.observed_report_sha256,
        verification.observed_ledger_sha256, verification.observed_state_sha256,
        verification.observed_revision, verification.hypothesis_id,
        verification.evidence_addresses, verification.accepted_evidence_addresses,
        verification.decision_payload);
    CHECK(!is_authoritative_evidence_revision(foreign));
}

void check_currentness_is_separate(const Artifacts& artifacts) {
    const EvidenceRevisionContract wrong_hash(
        artifacts.contract.report_sha256, std::string(64, '0'),
        artifacts.contract.state_sha256, artifacts.contract.accumulator_revision,
        artifacts.contract.hypothesis_id);
    const EvidenceRevisionContract stale(
        artifacts.contract.report_sha256, artifacts.contract.ledger_sha256,
        artifacts.contract.state_sha256, 3, artifacts.contract.hypothesis_id);

    const auto hash_result = verify_evidence_revision(
        artifacts.report, artifacts.ledger, artifacts.state, wrong_hash);
    const auto stale_result = verify_evidence_revision(
        artifacts.report, artifacts.ledger, artifacts.state, stale);
    CHECK(!hash_result.evidence_current);
    CHECK(hash_result.accumulator_revision_current);
    CHECK(!check_value(hash_result, "ledger_hash_matches"));
    CHECK(check_value(hash_result, "revision_matches_contract"));
    CHECK(stale_result.evidence_current);
    CHECK(!stale_result.accumulator_revision_current);
    CHECK(check_value(stale_result, "ledger_hash_matches"));
    CHECK(!check_value(stale_result, "revision_matches_contract"));
    CHECK(is_authoritative_evidence_revision(hash_result));
    CHECK(is_authoritative_evidence_revision(stale_result));
}

void check_contract_validation() {
    bool rejected = false;
    try {
        static_cast<void>(EvidenceRevisionContract(
            std::string(64, 'A'), std::string(64, '0'), std::string(64, '0'),
            1, "hypothesis"));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    CHECK(rejected);
}

void check_missing_accumulator_fails_closed_without_mint_failure(
    const Artifacts& artifacts) {
    const auto report = artifacts.root / "missing-accumulator.json";
    write(report, "{}");
    const EvidenceRevisionContract contract(
        sha256_file(report), artifacts.contract.ledger_sha256,
        artifacts.contract.state_sha256, artifacts.contract.accumulator_revision,
        artifacts.contract.hypothesis_id);
    const auto verification = verify_evidence_revision(
        report, artifacts.ledger, artifacts.state, contract);
    CHECK(!verification.evidence_current);
    CHECK(!verification.accumulator_revision_current);
    CHECK(is_authoritative_evidence_revision(verification));
    CHECK(!check_value(verification, "report_passed_and_accepted"));
    CHECK(!check_value(verification, "revision_matches_contract"));
}

}  // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() /
        ("swegca-evidence-revision-" + std::to_string(std::rand()));
    const Artifacts artifacts(root);
    check_pinned_artifacts(artifacts);
    check_currentness_is_separate(artifacts);
    check_contract_validation();
    check_missing_accumulator_fails_closed_without_mint_failure(artifacts);
    std::cout << "PASS immutable evidence revision verification and private capability\n";
}
