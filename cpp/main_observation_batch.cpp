#include "main_observation_batch.hpp"

#include "digest.hpp"
#include "memory_episode.hpp"
#include "memory_vrs_pair.hpp"
#include "observation.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace swegca::vrs {
namespace {

// SWEGCA: src/swegca_vrs2/store.py@7536139:384-387
std::string fold_graph_snapshot(std::string_view prior,
                                std::string_view fingerprint) {
    Json::Array parts;
    parts.emplace_back(std::string(prior));
    parts.emplace_back(std::string(fingerprint));
    return sha256_hex(Json(std::move(parts)).canonical());
}

// Detached node addresses make earlier rows in this physical transaction
// visible to the next source Graph.append without publishing them.
// SWEGCA: src/swegca_vrs2/store.py@7536139:384-399
class PendingGraphNodes final : public GraphNodeDirectory {
public:
    PendingGraphNodes(const GraphNodeDirectory& published,
                      const EventVrsInputView& source)
        : published_(published), source_(&source), published_count_(
              published.node_count()) {
        published_.require_source(source);
    }

    // SWEGCA: src/swegca_vrs2/store.py@7536139:178-202
    void require_source(const EventVrsInputView& source) const override {
        if (&source != source_ || source.snapshot_id() != source_->snapshot_id())
            throw std::runtime_error("pending graph node generation changed");
    }

    // SWEGCA: src/swegca_vrs2/store.py@7536139:196-202
    [[nodiscard]] std::uint64_t node_count() const override {
        return published_count_ + names_.size();
    }

    // SWEGCA: src/swegca_vrs2/store.py@7536139:196-202
    [[nodiscard]] bool contains(std::string_view name) const override {
        return addresses_.contains(std::string(name)) || published_.contains(name);
    }

    // SWEGCA: src/swegca_vrs2/store.py@7536139:201-202
    [[nodiscard]] std::uint32_t address(std::string_view name) const override {
        const auto found = addresses_.find(std::string(name));
        return found == addresses_.end() ? published_.address(name) : found->second;
    }

    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_connectivity_regions.py@7536139:250-272
    [[nodiscard]] std::string name(std::uint32_t address) const override {
        if (address < published_count_) return published_.name(address);
        const auto local = static_cast<std::uint64_t>(address) - published_count_;
        if (local >= names_.size())
            throw std::out_of_range("pending graph node address missing");
        return names_[static_cast<std::size_t>(local)];
    }

    // SWEGCA: src/swegca_vrs2/store.py@7536139:193-262
    void advance(const GraphAppendPlan& plan,
                 const EventVrsInputView& parent,
                 const EventVrsInputView& successor) {
        require_source(parent);
        if (plan.parent_snapshot_id != parent.snapshot_id() ||
            successor.node_count() != node_count() + plan.new_nodes.size())
            throw std::runtime_error("pending graph node transition changed");
        for (const auto& [name, address] : plan.new_nodes) {
            if (address != node_count() || contains(name))
                throw std::runtime_error("pending graph node transition changed");
            addresses_.emplace(name, address);
            names_.push_back(name);
        }
        source_ = &successor;
    }

private:
    const GraphNodeDirectory& published_;
    const EventVrsInputView* source_;
    std::uint64_t published_count_;
    std::vector<std::string> names_;
    std::map<std::string, std::uint32_t> addresses_;
};

struct PendingRegionBinding {
    std::uint32_t component;
    std::uint32_t local;
};

// Detached region replacements follow the same per-row graph source chain.
// SWEGCA: src/swegca_vrs2/store.py@7536139:282-302
class PendingGraphRegions final : public GraphRegionDirectory {
public:
    PendingGraphRegions(const GraphRegionDirectory& published,
                        const EventVrsInputView& source)
        : published_(published), source_(&source) {
        published_.require_source(source);
    }

    // SWEGCA: src/swegca_vrs2/store.py@7536139:282-291
    void require_source(const EventVrsInputView& source) const override {
        if (&source != source_ || source.snapshot_id() != source_->snapshot_id())
            throw std::runtime_error("pending graph region generation changed");
    }

    // SWEGCA: src/swegca_vrs2/store.py@7536139:282-291
    [[nodiscard]] std::optional<std::uint32_t> component_for(
        std::uint32_t node) const override {
        const auto found = bindings_.find(node);
        if (found != bindings_.end()) return found->second.component;
        const auto component = published_.component_for(node);
        if (component && removed_.contains(*component)) return std::nullopt;
        return component;
    }

    // SWEGCA: src/swegca_vrs2/store.py@7536139:299-302
    [[nodiscard]] std::shared_ptr<const RegionTopologyView> topology_for(
        std::uint32_t component) const override {
        const auto found = topologies_.find(component);
        if (found != topologies_.end()) return found->second;
        if (removed_.contains(component)) return nullptr;
        return published_.topology_for(component);
    }

    // SWEGCA: src/swegca_vrs2/store.py@7536139:299-302
    [[nodiscard]] std::uint32_t local_address(
        std::uint32_t component, std::uint32_t node) const override {
        const auto found = bindings_.find(node);
        if (found != bindings_.end()) {
            if (found->second.component != component)
                throw std::runtime_error("pending graph region component changed");
            return found->second.local;
        }
        if (removed_.contains(component))
            throw std::runtime_error("pending graph region component changed");
        return published_.local_address(component, node);
    }

    // SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_connectivity_regions.py@7536139:235-248
    void require_memory_source(const PublishedHotIndex& memory) const override {
        published_.require_memory_source(memory);
    }

    // SWEGCA: src/swegca_vrs2/store.py@7536139:282-302
    void advance(const GraphRegionPlan& plan,
                 const EventVrsInputView& parent,
                 const EventVrsInputView& successor) {
        require_source(parent);
        if (!plan.source || !plan.topology || !plan.topology->converged() ||
            plan.topology->vrs_snapshot_id() != successor.snapshot_id() ||
            plan.source->component_id >= successor.node_count() ||
            plan.topology->term_count() != plan.source->nodes.size())
            throw std::runtime_error("pending graph region transition changed");
        for (const auto component : plan.old_components) {
            removed_.insert(component);
            topologies_.erase(component);
        }
        const auto component = plan.source->component_id;
        removed_.erase(component);
        topologies_.insert_or_assign(component, plan.topology);
        for (std::uint64_t local = 0; local < plan.topology->term_count(); ++local) {
            const auto address = plan.topology->term(
                static_cast<std::uint32_t>(local));
            if (address != plan.source->nodes[static_cast<std::size_t>(local)])
                throw std::runtime_error("pending graph region transition changed");
            bindings_.insert_or_assign(address, PendingRegionBinding{
                component, static_cast<std::uint32_t>(local)});
        }
        source_ = &successor;
    }

private:
    const GraphRegionDirectory& published_;
    const EventVrsInputView* source_;
    std::map<std::uint32_t, PendingRegionBinding> bindings_;
    std::map<std::uint32_t, std::shared_ptr<const RegionTopologyView>> topologies_;
    std::set<std::uint32_t> removed_;
};

}  // namespace

// SWEGCA: src/swegca_vrs2/store.py@7536139:371-404
MainObservationBatchPlan plan_main_observation_batch(
    std::span<const Json> batch, const HotIndexRead& published_memory,
    const MainOperationRead& published_operations,
    const GraphNodeDirectory& nodes, const GraphRegionDirectory& regions,
    std::shared_ptr<const ValidatedEventVrsInputs> current_graph,
    std::string_view expected_parent_pair) {
    if (batch.empty())
        throw std::runtime_error("observation batch must not be empty");
    if (!current_graph)
        throw std::runtime_error("main graph source missing");
    const auto& initial_graph = current_graph->require_validated_immutable();
    const auto parent_graph = initial_graph.snapshot_id();
    if (full_current_pair_snapshot_id(published_memory.snapshot_id(),
                                      parent_graph) != expected_parent_pair)
        throw std::runtime_error("main_batch_parent_pair_changed");
    std::vector<Json> rows;
    std::vector<std::string> fingerprints;
    rows.reserve(batch.size());
    fingerprints.reserve(batch.size());
    for (const auto& arguments : batch) {
        auto row = observation(arguments);
        fingerprints.push_back(sha256_hex(row.canonical()));
        rows.push_back(std::move(row));
    }
    HotIndexPending memory(published_memory);
    MainOperationPending operations(published_operations);
    PendingGraphNodes pending_nodes(nodes, initial_graph);
    PendingGraphRegions pending_regions(regions, initial_graph);
    MainObservationBatchPlan plan;
    plan.parent_pair_id = std::string(expected_parent_pair);
    plan.results.reserve(rows.size());
    for (std::size_t at = 0; at < rows.size(); ++at) {
        const auto request_id = rows[at].at("request_id").string();
        const auto& fingerprint = fingerprints[at];
        const auto historical = operations.check(request_id, fingerprint);
        if (historical) {
            std::optional<std::string> historical_id;
            if (!historical->episode_id.empty())
                historical_id = historical->episode_id;
            plan.results.push_back(MainBatchRowResult{
                rows[at], std::move(historical_id),
                std::optional<std::string>{historical->pair_snapshot_id},
                true, false});
            continue;
        }
        const auto [identifier, added] = memory.append(rows[at]);
        const auto journal_position = plan.journal_rows.size();
        if (added) {
            auto episode = episode_from_observation(rows[at]);
            if (episode.episode_id != identifier)
                throw std::runtime_error("main_batch_episode_changed");
            const auto& parent = current_graph->require_validated_immutable();
            auto append = plan_graph_append(
                episode, fold_graph_snapshot(parent.snapshot_id(), fingerprint),
                memory, pending_nodes, *current_graph);
            auto numerical = settle_graph_event(append, current_graph);
            auto region = prepare_graph_regions(
                episode, append, numerical, *current_graph, pending_regions);
            const auto& successor =
                numerical.settled->require_validated_immutable();
            pending_nodes.advance(append, parent, successor);
            pending_regions.advance(region, parent, successor);
            current_graph = numerical.settled;
            const auto pair = full_current_pair_snapshot_id(
                memory.snapshot_id(), successor.snapshot_id());
            plan.graph_transitions.push_back(MainGraphRowTransition{
                journal_position, memory.snapshot_id(), pair,
                std::move(append), std::move(numerical), std::move(region)});
        }
        const auto graph_snapshot =
            current_graph->require_validated_immutable().snapshot_id();
        const auto pair = full_current_pair_snapshot_id(
            memory.snapshot_id(), graph_snapshot);
        operations.stage(request_id, MainOperation{fingerprint, identifier, pair});
        plan.journal_rows.push_back(PendingJournalRow{
            request_id, rows[at].canonical(), fingerprint, pair});
        plan.results.push_back(MainBatchRowResult{
            rows[at], identifier, pair, false, added});
    }
    plan.memory_snapshot_id = memory.snapshot_id();
    plan.graph_snapshot_id =
        current_graph->require_validated_immutable().snapshot_id();
    plan.memory_additions = memory.plans();
    if (plan.memory_additions.size() != plan.graph_transitions.size())
        throw std::runtime_error("main_batch_memory_plan_changed");
    plan.pair_snapshot_id = full_current_pair_snapshot_id(
        plan.memory_snapshot_id, plan.graph_snapshot_id);
    if (!plan.journal_rows.empty() &&
        plan.journal_rows.back().pair_id != plan.pair_snapshot_id)
        throw std::runtime_error("main_batch_pair_chain_changed");
    return plan;
}

}  // namespace swegca::vrs
