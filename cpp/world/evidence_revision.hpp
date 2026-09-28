#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace swegca::world {

class EvidenceRevisionContract final {
public:
    EvidenceRevisionContract(std::string report_sha256,
                             std::string ledger_sha256,
                             std::string state_sha256,
                             std::int64_t accumulator_revision,
                             std::string hypothesis_id);

    const std::string report_sha256;
    const std::string ledger_sha256;
    const std::string state_sha256;
    const std::int64_t accumulator_revision;
    const std::string hypothesis_id;
};

using RevisionPayloadValue = std::variant<
    std::nullptr_t, bool, std::int64_t, double, std::string,
    std::vector<std::string>>;

class EvidenceRevisionVerification final {
public:
    using Check = std::pair<std::string, bool>;
    using PayloadEntry = std::pair<std::string, RevisionPayloadValue>;

    // Public construction is an immutable data reconstruction only. It never
    // receives the private process-local verification capability.
    EvidenceRevisionVerification(
        bool evidence_current, bool accumulator_revision_current,
        std::vector<Check> checks, std::string observed_report_sha256,
        std::string observed_ledger_sha256, std::string observed_state_sha256,
        std::optional<std::int64_t> observed_revision,
        std::string hypothesis_id, std::vector<std::string> evidence_addresses,
        std::vector<std::string> accepted_evidence_addresses,
        std::vector<PayloadEntry> decision_payload);

    const bool evidence_current;
    const bool accumulator_revision_current;
    const std::vector<Check> checks;
    const std::string observed_report_sha256;
    const std::string observed_ledger_sha256;
    const std::string observed_state_sha256;
    const std::optional<std::int64_t> observed_revision;
    const std::string hypothesis_id;
    const std::vector<std::string> evidence_addresses;
    const std::vector<std::string> accepted_evidence_addresses;
    const std::vector<PayloadEntry> decision_payload;

private:
    EvidenceRevisionVerification(
        bool evidence_current, bool accumulator_revision_current,
        std::vector<Check> checks, std::string observed_report_sha256,
        std::string observed_ledger_sha256, std::string observed_state_sha256,
        std::optional<std::int64_t> observed_revision,
        std::string hypothesis_id, std::vector<std::string> evidence_addresses,
        std::vector<std::string> accepted_evidence_addresses,
        std::vector<PayloadEntry> decision_payload,
        std::shared_ptr<const void> authority);

    const std::shared_ptr<const void> authority_;

    friend EvidenceRevisionVerification verify_evidence_revision(
        const std::filesystem::path&, const std::filesystem::path&,
        const std::filesystem::path&, const EvidenceRevisionContract&);
    friend bool is_authoritative_evidence_revision(
        const EvidenceRevisionVerification&) noexcept;
};

[[nodiscard]] std::string sha256_file(const std::filesystem::path& path);

[[nodiscard]] EvidenceRevisionVerification verify_evidence_revision(
    const std::filesystem::path& report_path,
    const std::filesystem::path& ledger_path,
    const std::filesystem::path& state_path,
    const EvidenceRevisionContract& contract);

[[nodiscard]] bool is_authoritative_evidence_revision(
    const EvidenceRevisionVerification& verification) noexcept;

}  // namespace swegca::world
