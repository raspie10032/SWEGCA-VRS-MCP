#include "world/vrs_generation_rebind.hpp"

#include "swegca_architecture/sha256.hpp"
#include "world/semantic_vrs_ingress.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <span>
#include <stdexcept>

namespace swegca::world {
namespace {

bool digest_id(const std::string_view value) {
    return value.size() == 64 && std::ranges::all_of(value, [](const unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

std::string hex(const architecture::DigestBytes& value) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result(value.size() * 2, '0');
    for (std::size_t i = 0; i < value.size(); ++i) {
        const auto byte = std::to_integer<unsigned>(value[i]);
        result[2 * i] = digits[byte >> 4U];
        result[2 * i + 1] = digits[byte & 15U];
    }
    return result;
}

std::string digest(const std::string_view value) {
    return hex(architecture::Sha256::of(
        std::as_bytes(std::span(value.data(), value.size()))));
}

JsonValue strings(const std::vector<std::string>& values) {
    JsonValue::Array result;
    for (const auto& value : values) result.emplace_back(value);
    return result;
}

bool is_vrs_identifier(const std::string_view identifier) {
    return std::ranges::any_of(vrs_virtual_episode_prefixes,
        [&](const auto prefix) { return identifier.starts_with(prefix); });
}

}  // namespace

VrsGenerationBoundMemoryIndex::VrsGenerationBoundMemoryIndex(
    std::vector<std::shared_ptr<const HotMemoryIndex>> ordinary,
    std::shared_ptr<const VrsHotMemorySource> vrs,
    std::string effective, std::vector<std::string> replaced)
    : ordinary_sources(std::move(ordinary)), vrs_source(std::move(vrs)),
      effective_vrs_snapshot_id(std::move(effective)),
      replaced_vrs_source_snapshot_ids(std::move(replaced)) {
    if (!vrs_source || vrs_source->lookup_requires_io() ||
        !digest_id(effective_vrs_snapshot_id) || replaced_vrs_source_snapshot_ids.empty() ||
        std::ranges::any_of(replaced_vrs_source_snapshot_ids,
            [](const auto& value) { return !digest_id(value); }) ||
        std::ranges::find(replaced_vrs_source_snapshot_ids,
            std::string(vrs_source->snapshot_id())) != replaced_vrs_source_snapshot_ids.end())
        throw std::invalid_argument("VRS source generation did not change");
    for (const auto& source : ordinary_sources)
        if (!source || source->lookup_requires_io() ||
            std::dynamic_pointer_cast<const VrsHotMemorySource>(source))
            throw std::invalid_argument("ordinary hot-memory source boundary changed");
    for (const auto& outcome : memory_outcomes) {
        auto count = vrs_source->outcome_counts().at(outcome);
        for (const auto& source : ordinary_sources) count += source->outcome_counts().at(outcome);
        outcome_counts_[outcome] = count;
    }
    episode_count_ = vrs_source->episode_count();
    JsonValue::Array ordinary_ids;
    for (const auto& source : ordinary_sources) {
        episode_count_ += source->episode_count();
        ordinary_ids.emplace_back(std::string(source->snapshot_id()));
    }
    snapshot_id_ = digest(semantic_canonical_json(JsonValue::Object{
        {"effective_vrs_snapshot_id", effective_vrs_snapshot_id},
        {"ordinary_source_snapshot_ids", std::move(ordinary_ids)},
        {"replaced_vrs_source_snapshot_ids", strings(replaced_vrs_source_snapshot_ids)},
        {"schema_version", "rozephine-vrs-generation-bound-memory-v1"},
        {"vrs_source_snapshot_id", std::string(vrs_source->snapshot_id())}}));
}

std::string_view VrsGenerationBoundMemoryIndex::snapshot_id() const noexcept { return snapshot_id_; }
std::size_t VrsGenerationBoundMemoryIndex::episode_count() const noexcept { return episode_count_; }
const std::map<std::string, std::size_t, std::less<>>&
VrsGenerationBoundMemoryIndex::outcome_counts() const noexcept { return outcome_counts_; }

const MemoryEpisode& VrsGenerationBoundMemoryIndex::episode(
    const std::string_view identifier) const {
    if (is_vrs_identifier(identifier)) return vrs_source->episode(identifier);
    for (const auto& source : ordinary_sources) {
        try { return source->episode(identifier); }
        catch (const std::out_of_range&) {}
    }
    throw std::out_of_range(std::string(identifier));
}

std::vector<std::string> VrsGenerationBoundMemoryIndex::episode_ids_for_cue(
    const std::string_view cue) const {
    std::set<std::string, std::less<>> result;
    auto vrs = vrs_source->episode_ids_for_cue(cue);
    result.insert(vrs.begin(), vrs.end());
    for (const auto& source : ordinary_sources) {
        auto ids = source->episode_ids_for_cue(cue);
        result.insert(ids.begin(), ids.end());
    }
    return {result.begin(), result.end()};
}

std::vector<std::string> VrsGenerationBoundMemoryIndex::iter_episode_ids() const {
    std::vector<std::string> result;
    std::set<std::string, std::less<>> seen;
    for (const auto& source : ordinary_sources) {
        for (auto id : source->iter_episode_ids()) {
            if (!seen.insert(id).second)
                throw std::invalid_argument("rebound hot-memory episode identity overlaps");
            result.push_back(std::move(id));
        }
    }
    for (auto id : vrs_source->iter_episode_ids()) {
        if (!seen.insert(id).second)
            throw std::invalid_argument("rebound hot-memory episode identity overlaps");
        result.push_back(std::move(id));
    }
    return result;
}

std::vector<std::shared_ptr<const HotMemoryIndex>> vrs_leaf_sources(
    const std::shared_ptr<const HotMemoryIndex>& index) {
    if (!index) throw std::invalid_argument("hot memory source required");
    if (const auto composite = std::dynamic_pointer_cast<const CompositeMemoryActivationIndex>(index)) {
        std::vector<std::shared_ptr<const HotMemoryIndex>> result;
        for (const auto& source : composite->sources) {
            auto leaves = vrs_leaf_sources(source);
            result.insert(result.end(), leaves.begin(), leaves.end());
        }
        return result;
    }
    if (const auto bound = std::dynamic_pointer_cast<const VrsGenerationBoundMemoryIndex>(index)) {
        auto result = bound->ordinary_sources;
        result.push_back(bound->vrs_source);
        return result;
    }
    return {index};
}

ReboundFullCurrentVrs rebind_full_current_vrs_source(
    const FullCurrentMemoryVrsSnapshot& pair,
    std::shared_ptr<const VrsHotMemorySource> replacement,
    std::string report_sha256) {
    if (!replacement) throw std::invalid_argument("replacement VRS source required");
    std::ranges::transform(report_sha256, report_sha256.begin(),
        [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (!digest_id(report_sha256))
        throw std::invalid_argument("replacement VRS report must be a SHA-256 digest");
    if (report_sha256 != pair.vrs_snapshot_id)
        throw std::invalid_argument("replacement VRS source is not bound to the pair generation");
    const auto leaves = vrs_leaf_sources(pair.memory);
    std::vector<std::shared_ptr<const HotMemoryIndex>> ordinary;
    std::vector<std::shared_ptr<const VrsHotMemorySource>> old_vrs;
    for (const auto& source : leaves) {
        if (const auto vrs = std::dynamic_pointer_cast<const VrsHotMemorySource>(source))
            old_vrs.push_back(vrs);
        else ordinary.push_back(source);
    }
    if (old_vrs.empty())
        throw std::invalid_argument("full-current memory has no replaceable VRS source");
    if (std::ranges::any_of(old_vrs, [&](const auto& old) { return old.get() == replacement.get(); }))
        throw std::invalid_argument("replacement VRS source is already installed");
    std::vector<std::string> replaced;
    for (const auto& old : old_vrs) replaced.emplace_back(old->snapshot_id());
    auto memory = std::make_shared<const VrsGenerationBoundMemoryIndex>(
        ordinary, replacement, report_sha256, replaced);
    FullCurrentMemoryVrsSnapshot rebound(memory, report_sha256);
    bool shared = true;
    for (const auto& source : ordinary)
        shared = shared && std::ranges::any_of(memory->ordinary_sources,
            [&](const auto& candidate) { return candidate.get() == source.get(); });
    VrsGenerationRebindReceipt receipt;
    receipt.previous_pair_snapshot_id = pair.snapshot_id;
    receipt.replacement_pair_snapshot_id = rebound.snapshot_id;
    receipt.previous_memory_snapshot_id = std::string(pair.memory->snapshot_id());
    receipt.replacement_memory_snapshot_id = std::string(memory->snapshot_id());
    receipt.effective_vrs_snapshot_id = report_sha256;
    receipt.replaced_vrs_source_snapshot_ids = std::move(replaced);
    receipt.replacement_vrs_source_snapshot_id = std::string(replacement->snapshot_id());
    receipt.ordinary_source_count = ordinary.size();
    receipt.ordinary_sources_shared_by_identity = shared;
    return {std::move(rebound), std::move(receipt)};
}

}  // namespace swegca::world
