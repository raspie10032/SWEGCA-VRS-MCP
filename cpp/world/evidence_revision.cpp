#include "world/evidence_revision.hpp"

#include "swegca_architecture/sha256.hpp"
#include "transport/json.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <memory_resource>
#include <set>
#include <stdexcept>
#include <string_view>

namespace swegca::world {
namespace {

const std::shared_ptr<const void>& revision_authority() {
    static const auto token = std::static_pointer_cast<const void>(
        std::make_shared<const int>(0));
    return token;
}

void validate_sha256(const std::string& value, const char* name) {
    const bool valid = value.size() == 64 &&
        std::all_of(value.begin(), value.end(), [](const char character) {
            return (character >= '0' && character <= '9') ||
                   (character >= 'a' && character <= 'f');
        });
    if (!valid) throw std::invalid_argument(
        std::string(name) + " must be a lowercase SHA-256 digest");
}

std::string hex_digest(const architecture::DigestBytes& digest) {
    constexpr char digits[] = "0123456789abcdef";
    std::string out(digest.size() * 2, '0');
    for (std::size_t index = 0; index != digest.size(); ++index) {
        const auto value = std::to_integer<unsigned>(digest[index]);
        out[index * 2] = digits[value >> 4U];
        out[index * 2 + 1] = digits[value & 15U];
    }
    return out;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("unable to open evidence artifact");
    std::string data((std::istreambuf_iterator<char>(input)),
                     std::istreambuf_iterator<char>());
    if (input.bad()) throw std::runtime_error("unable to read evidence artifact");
    return data;
}

const transport::Json* member(const transport::Json& object,
                              const std::string_view key) noexcept {
    return object.find(key);
}

bool is_true(const transport::Json* value) noexcept {
    return value != nullptr && value->kind == transport::Json::Kind::boolean &&
           value->scalar == "true";
}

bool string_equals(const transport::Json* value,
                   const std::string_view expected) noexcept {
    return value != nullptr && value->kind == transport::Json::Kind::string &&
           value->scalar == expected;
}

std::optional<std::int64_t> json_integer(const transport::Json* value) {
    if (value == nullptr) return std::nullopt;
    if (value->kind == transport::Json::Kind::boolean)
        return value->scalar == "true" ? 1 : 0;
    if (value->kind != transport::Json::Kind::number ||
        value->scalar.find_first_of(".eE") != std::string_view::npos)
        return std::nullopt;
    std::int64_t result = 0;
    const auto* begin = value->scalar.data();
    const auto* end = begin + value->scalar.size();
    const auto parsed = std::from_chars(begin, end, result);
    if (parsed.ec != std::errc{} || parsed.ptr != end) return std::nullopt;
    return result;
}

std::optional<double> python_float(const transport::Json* value) {
    if (value == nullptr) return std::nullopt;
    if (value->kind == transport::Json::Kind::boolean)
        return value->scalar == "true" ? 1.0 : 0.0;
    std::string text;
    if (value->kind == transport::Json::Kind::number ||
        value->kind == transport::Json::Kind::string)
        text.assign(value->scalar);
    else return std::nullopt;
    double result = 0.0;
    const auto* begin = text.data();
    const auto* end = begin + text.size();
    const auto parsed = std::from_chars(begin, end, result);
    if (parsed.ec != std::errc{} || parsed.ptr != end) return std::nullopt;
    return result;
}

bool close_absolute(const std::optional<double> left,
                    const std::optional<double> right) noexcept {
    return left && right && std::isfinite(*left) && std::isfinite(*right) &&
           std::abs(*left - *right) <= 1e-12;
}

bool python_numeric_equal(const transport::Json* value,
                          const std::int64_t expected) {
    if (const auto integer = json_integer(value)) return *integer == expected;
    if (value == nullptr || value->kind != transport::Json::Kind::number)
        return false;
    const auto number = python_float(value);
    return number && *number == static_cast<double>(expected);
}

RevisionPayloadValue payload_value(const transport::Json* value) {
    if (value == nullptr || value->kind == transport::Json::Kind::null)
        return nullptr;
    if (value->kind == transport::Json::Kind::boolean)
        return value->scalar == "true";
    if (value->kind == transport::Json::Kind::string)
        return std::string(value->scalar);
    if (value->kind == transport::Json::Kind::number) {
        if (const auto integer = json_integer(value)) return *integer;
        const auto number = python_float(value);
        if (!number) throw std::invalid_argument("invalid decision payload number");
        return *number;
    }
    throw std::invalid_argument("decision payload field has unsupported JSON type");
}

struct LedgerRow final {
    bool hypothesis_matches{false};
    bool update_applied{false};
    bool update_reason_applied{false};
    std::string world_hash_before;
    std::string world_hash_after;
    std::optional<std::int64_t> integer_revision;
    std::string revision_lexeme;
    std::string decision;
    std::optional<double> posterior_after;
    std::optional<double> causal_lower_bound;
    std::vector<std::string> evidence_addresses;
};

LedgerRow decode_row(const transport::Json& row,
                     const std::string& hypothesis_id) {
    if (row.kind != transport::Json::Kind::object)
        throw std::invalid_argument("evidence ledger must contain JSON objects");
    LedgerRow out;
    out.hypothesis_matches = string_equals(member(row, "hypothesis_id"), hypothesis_id);
    out.update_applied = is_true(member(row, "update_applied"));
    out.update_reason_applied = string_equals(member(row, "update_reason"), "applied");
    const auto copy_string = [&](const char* key) {
        const auto* value = member(row, key);
        return value != nullptr && value->kind == transport::Json::Kind::string
            ? std::string(value->scalar) : std::string();
    };
    out.world_hash_before = copy_string("world_hash_before");
    out.world_hash_after = copy_string("world_hash_after");
    const auto* revision = member(row, "revision");
    out.integer_revision = json_integer(revision);
    if (revision != nullptr) {
        out.revision_lexeme.assign(revision->scalar);
    }
    out.decision = copy_string("decision");
    out.posterior_after = python_float(member(row, "posterior_after"));
    out.causal_lower_bound = python_float(member(row, "causal_lower_bound"));
    const auto* addresses = member(row, "evidence_addresses");
    if (addresses != nullptr && addresses->kind == transport::Json::Kind::array) {
        for (const auto& address : addresses->values)
            if (address.kind == transport::Json::Kind::string && !address.scalar.empty())
                out.evidence_addresses.emplace_back(address.scalar);
    }
    return out;
}

}  // namespace

EvidenceRevisionContract::EvidenceRevisionContract(
    std::string report_sha256_value, std::string ledger_sha256_value,
    std::string state_sha256_value, const std::int64_t accumulator_revision_value,
    std::string hypothesis_id_value)
    : report_sha256(std::move(report_sha256_value)),
      ledger_sha256(std::move(ledger_sha256_value)),
      state_sha256(std::move(state_sha256_value)),
      accumulator_revision(accumulator_revision_value),
      hypothesis_id(std::move(hypothesis_id_value)) {
    validate_sha256(report_sha256, "report_sha256");
    validate_sha256(ledger_sha256, "ledger_sha256");
    validate_sha256(state_sha256, "state_sha256");
    if (accumulator_revision <= 0)
        throw std::invalid_argument("accumulator_revision must be positive");
    if (hypothesis_id.empty())
        throw std::invalid_argument("hypothesis_id must be nonempty");
}

EvidenceRevisionVerification::EvidenceRevisionVerification(
    const bool evidence_current_value,
    const bool accumulator_revision_current_value,
    std::vector<Check> checks_value, std::string observed_report_sha256_value,
    std::string observed_ledger_sha256_value,
    std::string observed_state_sha256_value,
    std::optional<std::int64_t> observed_revision_value,
    std::string hypothesis_id_value, std::vector<std::string> evidence_addresses_value,
    std::vector<std::string> accepted_evidence_addresses_value,
    std::vector<PayloadEntry> decision_payload_value)
    : EvidenceRevisionVerification(
          evidence_current_value, accumulator_revision_current_value,
          std::move(checks_value), std::move(observed_report_sha256_value),
          std::move(observed_ledger_sha256_value),
          std::move(observed_state_sha256_value), observed_revision_value,
          std::move(hypothesis_id_value), std::move(evidence_addresses_value),
          std::move(accepted_evidence_addresses_value),
          std::move(decision_payload_value), {}) {}

EvidenceRevisionVerification::EvidenceRevisionVerification(
    const bool evidence_current_value,
    const bool accumulator_revision_current_value,
    std::vector<Check> checks_value, std::string observed_report_sha256_value,
    std::string observed_ledger_sha256_value,
    std::string observed_state_sha256_value,
    std::optional<std::int64_t> observed_revision_value,
    std::string hypothesis_id_value, std::vector<std::string> evidence_addresses_value,
    std::vector<std::string> accepted_evidence_addresses_value,
    std::vector<PayloadEntry> decision_payload_value,
    std::shared_ptr<const void> authority)
    : evidence_current(evidence_current_value),
      accumulator_revision_current(accumulator_revision_current_value),
      checks(std::move(checks_value)),
      observed_report_sha256(std::move(observed_report_sha256_value)),
      observed_ledger_sha256(std::move(observed_ledger_sha256_value)),
      observed_state_sha256(std::move(observed_state_sha256_value)),
      observed_revision(observed_revision_value),
      hypothesis_id(std::move(hypothesis_id_value)),
      evidence_addresses(std::move(evidence_addresses_value)),
      accepted_evidence_addresses(std::move(accepted_evidence_addresses_value)),
      decision_payload(std::move(decision_payload_value)),
      authority_(std::move(authority)) {}

std::string sha256_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("unable to open evidence artifact");
    architecture::Sha256 digest;
    std::vector<std::byte> buffer(1024U * 1024U);
    while (input) {
        input.read(reinterpret_cast<char*>(buffer.data()),
                   static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0) digest.update(std::span<const std::byte>(
            buffer.data(), static_cast<std::size_t>(count)));
    }
    if (!input.eof()) throw std::runtime_error("unable to read evidence artifact");
    return hex_digest(digest.finish());
}

EvidenceRevisionVerification verify_evidence_revision(
    const std::filesystem::path& report_path,
    const std::filesystem::path& ledger_path,
    const std::filesystem::path& state_path,
    const EvidenceRevisionContract& contract) {
    const std::string report_hash = sha256_file(report_path);
    const std::string ledger_hash = sha256_file(ledger_path);
    const std::string state_hash = sha256_file(state_path);
    const std::string report_text = read_file(report_path);
    const std::string ledger_text = read_file(ledger_path);

    std::pmr::monotonic_buffer_resource report_memory;
    const auto report = transport::parse_json(report_text, report_memory);
    if (report.kind != transport::Json::Kind::object)
        throw std::invalid_argument("evidence report must be a JSON object");
    transport::Json empty_accumulator(&report_memory);
    empty_accumulator.kind = transport::Json::Kind::object;
    const auto* accumulator_section = member(report, "accumulator");
    const transport::Json* accumulator = &empty_accumulator;
    if (accumulator_section != nullptr &&
        accumulator_section->kind == transport::Json::Kind::object) {
        const auto* decision = member(*accumulator_section, "decision");
        accumulator = decision == nullptr ? accumulator_section : decision;
    }
    if (accumulator->kind != transport::Json::Kind::object)
        throw std::invalid_argument("evidence report accumulator must be an object");

    std::vector<LedgerRow> rows;
    std::size_t begin = 0;
    while (begin <= ledger_text.size()) {
        const auto end = ledger_text.find('\n', begin);
        const auto line = std::string_view(ledger_text).substr(
            begin, end == std::string::npos ? ledger_text.size() - begin : end - begin);
        const auto first = line.find_first_not_of(" \t\r\n");
        if (first != std::string_view::npos) {
            std::pmr::monotonic_buffer_resource row_memory;
            const auto row = transport::parse_json(line, row_memory);
            rows.push_back(decode_row(row, contract.hypothesis_id));
        }
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    if (rows.empty())
        throw std::invalid_argument("evidence ledger must contain JSON objects");

    const auto& final = rows.back();
    const auto* report_revision_value = member(*accumulator, "revision");
    const std::optional<std::int64_t> observed_revision = final.integer_revision;
    bool revisions_contiguous = true;
    for (std::size_t index = 0; index != rows.size(); ++index) {
        // Python list equality treats integral floats and bools as equal to ints.
        std::pmr::monotonic_buffer_resource value_memory;
        const std::string& lexeme = rows[index].revision_lexeme;
        if (lexeme.empty()) { revisions_contiguous = false; break; }
        const auto parsed = transport::parse_json(lexeme, value_memory);
        if (!python_numeric_equal(&parsed, static_cast<std::int64_t>(index + 1))) {
            revisions_contiguous = false;
            break;
        }
    }
    std::set<std::string> world_hashes;
    bool all_hypotheses = true;
    bool all_updates = true;
    std::set<std::string> accepted_addresses;
    for (const auto& row : rows) {
        world_hashes.insert(row.world_hash_before);
        world_hashes.insert(row.world_hash_after);
        all_hypotheses = all_hypotheses && row.hypothesis_matches;
        all_updates = all_updates && row.update_applied && row.update_reason_applied;
        accepted_addresses.insert(row.evidence_addresses.begin(),
                                  row.evidence_addresses.end());
    }
    const auto* lineage = member(report, "lineage");
    if (lineage != nullptr && lineage->kind != transport::Json::Kind::object)
        throw std::invalid_argument("evidence report lineage must be an object");
    const auto* report_world = lineage != nullptr &&
        lineage->kind == transport::Json::Kind::object
        ? member(*lineage, "state_sha256") : nullptr;
    const bool metrics_match = observed_revision.has_value() &&
        python_numeric_equal(report_revision_value, *observed_revision) &&
        string_equals(member(*accumulator, "status"), final.decision) &&
        close_absolute(final.posterior_after,
                       python_float(member(*accumulator, "posterior_mean"))) &&
        close_absolute(final.causal_lower_bound,
                       python_float(member(*accumulator, "causal_lower_bound")));

    std::vector<EvidenceRevisionVerification::Check> checks{
        {"report_hash_matches", report_hash == contract.report_sha256},
        {"ledger_hash_matches", ledger_hash == contract.ledger_sha256},
        {"state_hash_matches", state_hash == contract.state_sha256},
        {"report_passed_and_accepted",
            is_true(member(report, "passed")) &&
            string_equals(member(*accumulator, "status"), "accept")},
        {"hypothesis_matches", all_hypotheses},
        {"ledger_updates_applied", all_updates},
        {"ledger_world_hashes_match_state",
            world_hashes == std::set<std::string>{contract.state_sha256}},
        {"report_world_hash_matches_state",
            string_equals(report_world, contract.state_sha256)},
        {"ledger_revisions_contiguous", revisions_contiguous},
        {"revision_matches_contract",
            observed_revision == std::optional<std::int64_t>(contract.accumulator_revision) &&
            python_numeric_equal(report_revision_value, contract.accumulator_revision)},
        {"final_ledger_metrics_match_report", metrics_match},
    };
    const auto check = [&](const std::string_view key) {
        const auto found = std::find_if(checks.begin(), checks.end(),
            [&](const auto& item) { return item.first == key; });
        return found != checks.end() && found->second;
    };
    const bool evidence_current =
        check("report_hash_matches") && check("ledger_hash_matches") &&
        check("state_hash_matches") && check("report_passed_and_accepted") &&
        check("hypothesis_matches") && check("ledger_updates_applied") &&
        check("ledger_world_hashes_match_state") &&
        check("report_world_hash_matches_state");
    const bool revision_current =
        check("ledger_revisions_contiguous") &&
        check("revision_matches_contract") &&
        check("final_ledger_metrics_match_report");

    static constexpr std::string_view decision_fields[]{
        "status", "reason", "posterior_mean", "causal_lower_bound",
        "overall_upper_bound", "effective_sample_size", "source_diversity",
        "context_diversity", "regime_change_score", "revision"};
    std::vector<EvidenceRevisionVerification::PayloadEntry> payload;
    payload.reserve(std::size(decision_fields) + 2);
    for (const auto key : decision_fields)
        payload.emplace_back(std::string(key), payload_value(member(*accumulator, key)));
    std::vector<std::string> accepted(accepted_addresses.begin(), accepted_addresses.end());
    payload.emplace_back("hypothesis_id", contract.hypothesis_id);
    payload.emplace_back("evidence_addresses", accepted);
    return EvidenceRevisionVerification(
        evidence_current, revision_current, std::move(checks), report_hash,
        ledger_hash, state_hash, observed_revision, contract.hypothesis_id,
        {"sha256:" + report_hash, "sha256:" + ledger_hash, "sha256:" + state_hash},
        std::move(accepted), std::move(payload), revision_authority());
}

bool is_authoritative_evidence_revision(
    const EvidenceRevisionVerification& verification) noexcept {
    return verification.authority_ == revision_authority();
}

}  // namespace swegca::world
