#pragma once

#include "world/cognitive_state.hpp"
#include "world/evidence_accumulator.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view memory_promotion_source_sha256 =
    "336073a8eb69357d5761add74316a6f06719c61bbaeb93b82fd3e4005390d9e5";
inline constexpr std::string_view external_memory_source_sha256 =
    "a2a7e00960540c43c5bfef4ddf689fc4bedc491a3e85caa884efa6288cfa3941";
inline constexpr std::string_view versioned_memory_source_sha256 =
    "c1b1bb2302256b834583101b8f51e53656f389eceaa7a00a8c8e020c631596e1";
inline constexpr double verified_experience_promotion_strength = 1.0;

enum class MemoryTier : std::uint8_t { none, episodic, semantic, quarantined, retracted };

struct MemoryCandidate final {
    std::string hypothesis_id;
    std::string key;
    std::string value;
    std::vector<std::string> evidence_refs;
    std::string source_id;
    std::string source_revision;
    std::string timestamp;
    std::string license;
    std::string attribution;
    std::vector<std::string> retrieval_aliases;

    MemoryCandidate(std::string hypothesis_id, std::string key, std::string value,
                    std::vector<std::string> evidence_refs, std::string source_id,
                    std::string source_revision, std::string timestamp,
                    std::string license, std::string attribution,
                    std::vector<std::string> retrieval_aliases = {});
};

class MemoryPromotionDecision final {
public:
    [[nodiscard]] MemoryTier previous_tier() const noexcept;
    [[nodiscard]] MemoryTier next_tier() const noexcept;
    [[nodiscard]] std::string_view action() const noexcept;
    [[nodiscard]] std::string_view reason() const noexcept;
    [[nodiscard]] bool semantic_read_allowed() const noexcept;

private:
    MemoryPromotionDecision(MemoryTier previous_tier, MemoryTier next_tier,
                            std::string action, std::string reason,
                            bool semantic_read_allowed, bool authoritative);
    MemoryTier previous_tier_;
    MemoryTier next_tier_;
    std::string action_;
    std::string reason_;
    bool semantic_read_allowed_{};
    bool authoritative_{};
    friend MemoryPromotionDecision decide_memory_promotion(
        MemoryTier, const AccumulatorDecision&, bool, bool);
    friend void require_authoritative_promotion(const MemoryPromotionDecision&);
};

struct VRSExperiencePromotionDecision final {
    std::string snapshot_id;
    std::string connection_id;
    double previous_strength{};
    double current_strength{};
    std::string action;
    bool promoted{};
    bool semantic_evidence_allowed{};
    bool underlying_experience_preserved{true};
    bool action_authorized{false};
    bool persistent_write_authorized{false};

    VRSExperiencePromotionDecision(std::string snapshot_id,
        std::string connection_id, double previous_strength,
        double current_strength, std::string action, bool promoted,
        bool semantic_evidence_allowed,
        bool underlying_experience_preserved = true,
        bool action_authorized = false,
        bool persistent_write_authorized = false);
};

struct MemoryCandidateDocument final {
    std::string docid;
    std::string source_id;
    std::string source_revision;
    std::string page_id;
    std::string revision_id;
    std::string namespace_name;
    std::string timestamp;
    bool redirect{};
    std::string title;
    std::string text;
    std::string content_sha256;
    std::string duplicate_status;
    std::string source_url;
    std::string license;
    std::string attribution;
};
using ExternalMemoryDocument = MemoryCandidateDocument;

class MosaicExternalMemory final {
public:
    explicit MosaicExternalMemory(std::filesystem::path database,
                                  std::string tokenizer = "unicode61");
    [[nodiscard]] const std::filesystem::path& database() const noexcept;
    [[nodiscard]] std::string_view tokenizer() const noexcept;
    void upsert(const ExternalMemoryDocument& document);
    [[nodiscard]] std::vector<ExternalMemoryDocument> documents(
        std::optional<std::string_view> namespace_name = std::nullopt) const;
    [[nodiscard]] bool delete_document(std::string_view docid);

private:
    [[nodiscard]] void* connect(bool readonly) const;
    static void close(void* connection) noexcept;
    static void ensure_update_triggers(void* connection);
    static void refresh_duplicate_group(void* connection,
                                        std::string_view content_sha256);
    static void upsert_in(void* connection,
                          const ExternalMemoryDocument& document);
    static bool delete_in(void* connection, std::string_view docid);

    std::filesystem::path database_;
    std::string tokenizer_;
    friend class VersionedExternalMemory;
};

struct MemoryMutation final {
    std::string update_id;
    std::string operation;
    std::string docid;
    std::optional<std::string> supersedes_docid;
    std::string applied_at;
};

class VersionedExternalMemory final {
public:
    explicit VersionedExternalMemory(MosaicExternalMemory& memory,
                                     std::size_t maximum_search_candidates = 256);
    [[nodiscard]] constexpr std::size_t persistent_state_count() const noexcept { return 0; }
    [[nodiscard]] MemoryMutation upsert_verified(
        const ExternalMemoryDocument& document,
        const AccumulatorDecision& decision, std::string valid_from,
        std::string update_id,
        std::optional<std::string> supersedes_docid = std::nullopt,
        std::optional<std::string> valid_until = std::nullopt);

private:
    void ensure_schema();
    MosaicExternalMemory& memory_;
    std::size_t maximum_search_candidates_{};
};

struct MemoryPromotionApplication final {
    std::string action;
    MemoryTier next_tier{};
    bool episodic_present{};
    bool semantic_present{};
    bool semantic_read_allowed{};
};

[[nodiscard]] VRSExperiencePromotionDecision assess_vrs_experience_promotion(
    std::string snapshot_id, std::string connection_id,
    double previous_strength, double current_strength);
[[nodiscard]] MemoryPromotionDecision decide_memory_promotion(
    MemoryTier current_tier, const AccumulatorDecision& evidence,
    bool counterfactual_verified, bool provenance_complete);
void require_authoritative_promotion(const MemoryPromotionDecision& decision);
[[nodiscard]] MemoryCandidateDocument memory_candidate_document(
    const MemoryCandidate& candidate, MemoryTier tier,
    std::string docid = {});
[[nodiscard]] MemoryPromotionApplication apply_memory_promotion(
    MosaicExternalMemory& episodic_memory, MosaicExternalMemory& semantic_memory,
    const MemoryCandidate& candidate, const MemoryPromotionDecision& decision);
[[nodiscard]] MemoryMutation apply_verified_memory_update(
    VersionedExternalMemory& memory, const MemoryCandidate& candidate,
    const AccumulatorDecision& evidence,
    const MemoryPromotionDecision& promotion, std::string valid_from,
    std::string update_id,
    std::optional<std::string> supersedes_docid = std::nullopt,
    std::optional<std::string> valid_until = std::nullopt);
[[nodiscard]] std::string_view memory_tier_name(MemoryTier tier) noexcept;

}  // namespace swegca::world
