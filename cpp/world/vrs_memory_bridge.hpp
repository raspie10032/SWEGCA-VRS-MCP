#pragma once

#include "world/memory_activation.hpp"
#include "world/memory_promotion.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view vrs_memory_bridge_source_sha256 =
    "a7a47d0e6ecdb4b714e73c6e90a9cb78be733eaf28444785d0ff6c0db6674d04";

struct VrsEvidenceRequest final {
    std::size_t term_id{};
    std::string term;
    std::string requested_evidence;
    JsonValue::Object fields;
};

class VrsHotAddressIndex final {
public:
    VrsHotAddressIndex(
        std::map<std::string, std::vector<std::size_t>, std::less<>> term_ids_by_cue,
        std::vector<std::size_t> edge_offsets,
        std::vector<std::size_t> edge_ids,
        std::map<std::size_t, std::vector<std::size_t>> request_ids_by_term,
        std::size_t term_count, std::size_t edge_count);
    [[nodiscard]] std::vector<std::size_t> term_ids(std::string_view cue) const;
    [[nodiscard]] std::span<const std::size_t> edge_ids_for_term(std::size_t term_id) const;

    const std::map<std::string, std::vector<std::size_t>, std::less<>> term_ids_by_cue;
    const std::vector<std::size_t> edge_offsets;
    const std::vector<std::size_t> edge_ids;
    const std::map<std::size_t, std::vector<std::size_t>> request_ids_by_term;
    const std::size_t term_count;
    const std::size_t edge_count;
};

class VrsHotMemorySource : public HotMemoryIndex {
public:
    VrsHotMemorySource(
        std::vector<std::string> terms, std::vector<double> score,
        std::vector<std::int64_t> support, std::vector<std::int64_t> refute,
        std::vector<std::uint32_t> edge_source,
        std::vector<std::uint32_t> edge_target,
        std::vector<std::int8_t> edge_sign,
        std::vector<double> vrs_strength,
        std::vector<VrsEvidenceRequest> evidence_requests,
        std::shared_ptr<const VrsHotAddressIndex> address_index,
        std::string source_address,
        double promotion_threshold = verified_experience_promotion_strength);

    [[nodiscard]] std::string_view snapshot_id() const noexcept override;
    [[nodiscard]] std::size_t episode_count() const noexcept override;
    [[nodiscard]] const std::map<std::string, std::size_t, std::less<>>&
        outcome_counts() const noexcept override;
    [[nodiscard]] const MemoryEpisode& episode(std::string_view episode_id) const override;
    [[nodiscard]] std::vector<std::string> episode_ids_for_cue(
        std::string_view cue) const override;
    [[nodiscard]] std::vector<std::string> iter_episode_ids() const override;

    const std::vector<std::string> terms;
    const std::vector<double> score;
    const std::vector<std::int64_t> support;
    const std::vector<std::int64_t> refute;
    const std::vector<std::uint32_t> edge_source;
    const std::vector<std::uint32_t> edge_target;
    const std::vector<std::int8_t> edge_sign;
    const std::vector<double> vrs_strength;
    const std::vector<VrsEvidenceRequest> evidence_requests;
    const std::shared_ptr<const VrsHotAddressIndex> address_index;
    const std::string source_address;
    const double promotion_threshold;

protected:
    [[nodiscard]] virtual MemoryEpisode edge_episode(
        std::size_t edge_id, std::string episode_id) const;
    [[nodiscard]] MemoryEpisode term_episode(std::size_t term_id) const;
    [[nodiscard]] MemoryEpisode request_episode(std::size_t request_id) const;
    [[nodiscard]] const MemoryEpisode& retain(std::string key, MemoryEpisode value) const;

    std::string snapshot_id_;
    std::size_t episode_count_{};
    std::map<std::string, std::size_t, std::less<>> outcome_counts_;
    mutable std::map<std::string, std::shared_ptr<const MemoryEpisode>, std::less<>> retained_;
    mutable std::mutex retained_mutex_;
};

class CanonicalVrsHotMemorySource final : public VrsHotMemorySource {
public:
    CanonicalVrsHotMemorySource(
        std::vector<std::string> terms, std::vector<double> score,
        std::vector<std::int64_t> support, std::vector<std::int64_t> refute,
        std::vector<std::uint32_t> edge_source,
        std::vector<std::uint32_t> edge_target,
        std::vector<std::int8_t> edge_sign,
        std::vector<double> vrs_strength,
        std::vector<VrsEvidenceRequest> evidence_requests,
        std::shared_ptr<const VrsHotAddressIndex> address_index,
        std::string source_address, double promotion_threshold,
        std::vector<std::uint32_t> member_edge_to_group,
        std::vector<std::uint64_t> group_member_offsets,
        std::vector<std::uint32_t> group_member_edge_ids,
        std::string member_manifest_address);

    [[nodiscard]] std::size_t original_edge_count() const noexcept;
    [[nodiscard]] std::size_t canonical_group_for_member(std::size_t edge_id) const;
    [[nodiscard]] std::span<const std::uint32_t> member_edge_ids_for_group(
        std::size_t group_id) const;
    [[nodiscard]] const MemoryEpisode& episode(std::string_view episode_id) const override;
    [[nodiscard]] std::vector<std::string> episode_ids_for_cue(
        std::string_view cue) const override;
    [[nodiscard]] std::vector<std::string> iter_episode_ids() const override;

    const std::vector<std::uint32_t> member_edge_to_group;
    const std::vector<std::uint64_t> group_member_offsets;
    const std::vector<std::uint32_t> group_member_edge_ids;
    const std::string member_manifest_address;

protected:
    [[nodiscard]] MemoryEpisode edge_episode(
        std::size_t group_id, std::string episode_id) const override;
};

struct VrsQueryMemoryBundle final {
    std::shared_ptr<const HotMemoryIndex> index;
    std::map<std::string, std::vector<std::string>, std::less<>> query_cues;
    std::pair<std::size_t, std::size_t> selected_term_ids;
    std::pair<std::size_t, std::size_t> selected_edge_ids;
    std::vector<std::string> promotion_eligible_episode_ids;
    std::map<std::string, std::size_t, std::less<>> outcome_counts;
    std::string source_address;
    std::shared_ptr<const VrsHotMemorySource> vrs_source;
};

struct VrsQueryMemoryInput final {
    std::vector<std::string> terms;
    std::vector<double> score;
    std::vector<std::int64_t> support;
    std::vector<std::int64_t> refute;
    std::vector<std::uint32_t> edge_source;
    std::vector<std::uint32_t> edge_target;
    std::vector<std::int8_t> edge_sign;
    std::vector<double> vrs_strength;
    std::vector<VrsEvidenceRequest> evidence_requests;
    std::vector<std::string> queries;
    std::string source_address;
    double promotion_threshold{verified_experience_promotion_strength};
    std::vector<MemoryEpisode> additional_episodes;
    std::optional<std::vector<std::uint32_t>> member_edge_to_group;
    std::optional<std::vector<std::uint64_t>> group_member_offsets;
    std::optional<std::vector<std::uint32_t>> group_member_edge_ids;
    std::optional<std::string> member_manifest_address;
};

[[nodiscard]] VrsQueryMemoryBundle build_vrs_query_memory_bundle(
    VrsQueryMemoryInput input);
[[nodiscard]] std::string canonical_vrs_cue(std::string_view value);

}  // namespace swegca::world
