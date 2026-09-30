#pragma once

#include "world/memory_activation.hpp"
#include "world/syllogism.hpp"

#include <compare>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view syllogism_sources_source_sha256 =
    "7813eee965b1241ec8874c9eb962fcccac8c27d7f8c52183a8136516dfb1ffff";
inline constexpr std::string_view categorical_premises_schema =
    "rozephine-categorical-premises-v1";

struct PremiseAddress final {
    std::string episode_id;
    std::size_t step{};
    std::size_t statement{};
    friend auto operator<=>(const PremiseAddress&, const PremiseAddress&) = default;
};

struct SourcePremise final {
    PremiseAddress address;
    CategoricalStatement statement;
    std::string revision;
    std::vector<std::string> source_addresses;
    std::vector<std::string> evidence_refs;
    std::string outcome;
    std::string verification_state;
    bool grants_authority{};
};

struct SourceSyllogism final {
    std::string memory_snapshot_id;
    SourcePremise major;
    SourcePremise minor;
    CategoricalStatement conclusion;
    SyllogismForm form;
    bool conditional_on_premises{true};
    bool grants_authority{};
};

struct OpposedPremiseClaims final {
    SourcePremise premise;
    SourcePremise opposed;
    bool establishes_current_conflict{};
    bool grants_authority{};
};

class PremiseReadView final {
public:
    using Pair = std::pair<std::string, std::string>;
    using AddressMap = std::map<PremiseAddress, SourcePremise>;
    using PairMap = std::map<Pair, std::vector<PremiseAddress>>;
    using TermMap = std::map<std::string, std::vector<PremiseAddress>, std::less<>>;

    [[nodiscard]] static PremiseReadView from_indexes(
        std::string memory_snapshot_id,
        std::shared_ptr<const AddressMap> by_address,
        std::shared_ptr<const PairMap> by_pair,
        std::shared_ptr<const TermMap> by_term);

    [[nodiscard]] const SourcePremise& premise(const PremiseAddress& address) const;
    [[nodiscard]] std::vector<PremiseAddress> term_addresses(
        std::string_view term) const;
    [[nodiscard]] std::vector<std::optional<OpposedPremiseClaims>> opposition_steps(
        const PremiseAddress& address) const;
    [[nodiscard]] std::vector<SourceSyllogism> attempts(
        const PremiseAddress& major_address,
        const CategoricalStatement& conclusion) const;

    const std::string memory_snapshot_id;

private:
    friend class PremiseSegment;
    PremiseReadView(std::string memory_snapshot_id,
                    std::shared_ptr<const AddressMap> by_address,
                    std::shared_ptr<const PairMap> by_pair,
                    std::shared_ptr<const TermMap> by_term);

    const std::shared_ptr<const AddressMap> by_address_;
    const std::shared_ptr<const PairMap> by_pair_;
    const std::shared_ptr<const TermMap> by_term_;
};

class PremiseSegment final {
public:
    [[nodiscard]] PremiseReadView bind(
        const std::shared_ptr<const HotMemoryIndex>& memory) const;
    [[nodiscard]] const std::shared_ptr<const PremiseReadView::AddressMap>&
        address_index() const noexcept { return by_address_; }
    [[nodiscard]] const std::shared_ptr<const PremiseReadView::PairMap>&
        pair_index() const noexcept { return by_pair_; }
    [[nodiscard]] const std::shared_ptr<const PremiseReadView::TermMap>&
        term_index() const noexcept { return by_term_; }

    const std::string memory_snapshot_id;
    const std::vector<std::string> inspected_episode_ids;
    const std::vector<std::string> episodes_without_explicit_premises;

private:
    friend PremiseSegment prepare_premise_segment(
        std::shared_ptr<const HotMemoryIndex>,
        const std::vector<std::string>&,
        const std::function<void(const MemoryEpisode&)>&);
    PremiseSegment(std::shared_ptr<const HotMemoryIndex> memory,
                   std::shared_ptr<const PremiseReadView::AddressMap> by_address,
                   std::shared_ptr<const PremiseReadView::PairMap> by_pair,
                   std::shared_ptr<const PremiseReadView::TermMap> by_term,
                   std::vector<std::string> inspected,
                   std::vector<std::string> absent);

    const std::shared_ptr<const HotMemoryIndex> memory_;
    const std::shared_ptr<const PremiseReadView::AddressMap> by_address_;
    const std::shared_ptr<const PremiseReadView::PairMap> by_pair_;
    const std::shared_ptr<const PremiseReadView::TermMap> by_term_;
};

[[nodiscard]] PremiseSegment prepare_premise_segment(
    std::shared_ptr<const HotMemoryIndex> memory,
    const std::vector<std::string>& episode_ids,
    const std::function<void(const MemoryEpisode&)>& admitted_episode = {});

}  // namespace swegca::world
