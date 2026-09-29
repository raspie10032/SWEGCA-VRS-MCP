#include "world/semantic_event_append.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <utility>

namespace swegca::world {
namespace {

[[noreturn]] void reject(const char* reason) { throw std::invalid_argument(reason); }

[[nodiscard]] float stored_float16_strength(const float value) {
    const auto result = event_strength_from_float16_bits(
        event_strength_float16_bits(value));
    if (!std::isfinite(result)) reject("event strength exceeds durable f16 capacity");
    if ((value >= 1.0F) != (result >= 1.0F))
        reject("durable f16 rounding changes experience promotion");
    return result;
}

class StageTimer final {
public:
    void mark(std::string name) {
        const auto now = Clock::now();
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
            now - marked_).count();
        rows_.emplace_back(std::move(name),
            elapsed < 0 ? 0U : static_cast<std::uint64_t>(elapsed));
        marked_ = now;
    }

    [[nodiscard]] std::vector<std::pair<std::string, std::uint64_t>> take() {
        return std::move(rows_);
    }

private:
    using Clock = std::chrono::steady_clock;
    Clock::time_point marked_{Clock::now()};
    std::vector<std::pair<std::string, std::uint64_t>> rows_;
};

template<class Lineage, class AppendLineage>
SemanticEventAppend prepare_impl(
    std::shared_ptr<const EventSignalInputs> parent,
    std::shared_ptr<const TermAddressIndex> address_index,
    const Lineage& lineage,
    AppendLineage&& append_lineage,
    std::shared_ptr<const CanonicalEdgeAddressIndex> edge_address_index,
    const std::vector<SemanticSourceEpisode>& episodes,
    const std::vector<SemanticEncoding>& proposals,
    std::string snapshot_id) {
    if (!parent || !address_index || !edge_address_index)
        reject("bound numerical parent and term directory required");
    edge_address_index->require_source(parent->edges);
    (void)lineage.require_validated_immutable();
    if (address_index->size() != parent->score.size() ||
        parent->direct.size() != parent->score.size() ||
        parent->unresolved.size() != parent->score.size() ||
        lineage.group_count() != parent->edges.size() ||
        parent->strength.size() != parent->edges.size() ||
        snapshot_id == parent->snapshot_id)
        reject("semantic append parent generation or shape changed");

    StageTimer timer;
    auto semantic = prepare_semantic_graph_delta(
        address_index, parent->edges.size(), episodes, proposals);
    timer.mark("source_bound_semantic_delta");

    std::vector<float> appended_strength(semantic.edge_rows.size(), .75F);
    auto canonical = prepare_canonical_vrs_append_delta(
        parent->edges, parent->strength, lineage, semantic.edge_rows,
        appended_strength, *edge_address_index);
    timer.mark("canonical_changed_groups");

    auto successor_terms = address_index->append_shared(semantic.appended_terms);
    auto successor_lineage = append_lineage(
        canonical.appended_member_group_ids, canonical.new_group_rows.size());
    timer.mark("shared_terms_and_membership");

    const auto old_group_count = parent->edges.size();
    auto new_edges = canonical.new_group_rows;
    std::vector<float> new_strength(new_edges.size());
    std::vector<std::uint8_t> initialized(new_edges.size(), 0);
    std::vector<std::size_t> changed;
    std::vector<float> base_values;
    std::vector<float> strength_values;
    changed.reserve(canonical.group_updates.size());
    base_values.reserve(canonical.group_updates.size());
    strength_values.reserve(canonical.group_updates.size());
    for (const auto& update : canonical.group_updates) {
        const auto stored = stored_float16_strength(update.current_strength);
        if (update.group < old_group_count) {
            changed.push_back(update.group);
            base_values.push_back(update.base_strength);
            strength_values.push_back(stored);
        } else {
            const auto offset = static_cast<std::size_t>(update.group) - old_group_count;
            if (offset >= new_edges.size() || initialized[offset])
                reject("canonical semantic group update changed");
            new_edges[offset].vrs_strength = update.base_strength;
            new_strength[offset] = stored;
            initialized[offset] = 1;
        }
    }
    if (std::ranges::find(initialized, std::uint8_t{0}) != initialized.end())
        reject("canonical semantic group update missing");

    std::vector<float> appended_nodes(semantic.appended_terms.size(), 0.0F);
    std::vector<std::uint8_t> appended_unresolved(semantic.appended_terms.size(), 0);
    auto candidate = prepare_event_delta(
        parent, std::move(snapshot_id), appended_nodes, appended_nodes,
        appended_unresolved, new_edges, new_strength,
        changed, base_values, changed, strength_values);
    timer.mark("sparse_numeric_inputs");

    auto successor_edges = edge_address_index->advance_event_delta(*parent, *candidate);
    timer.mark("canonical_address_extension");
    return SemanticEventAppend(
        std::move(parent), std::move(candidate), std::move(successor_terms),
        std::move(successor_edges), std::move(successor_lineage),
        std::move(semantic), std::move(canonical), timer.take());
}

template<class Lineage, class AppendLineage>
SessionSemanticEventAppend prepare_session_impl(
    std::shared_ptr<const EventSignalInputs> parent,
    std::shared_ptr<const TermAddressIndex> address_index,
    const Lineage& lineage,
    AppendLineage&& append_lineage,
    std::shared_ptr<const CanonicalEdgeAddressIndex> edge_address_index,
    std::shared_ptr<const BoundSessionSemantics> session_binding,
    std::string snapshot_id,
    std::string parent_vrs_snapshot_id) {
    if (!parent || !address_index || !edge_address_index || !session_binding)
        reject("bound numerical parent and term directory required");
    edge_address_index->require_source(parent->edges);
    (void)lineage.require_validated_immutable();
    if (address_index->size() != parent->score.size() ||
        parent->direct.size() != parent->score.size() ||
        parent->unresolved.size() != parent->score.size() ||
        lineage.group_count() != parent->edges.size() ||
        parent->strength.size() != parent->edges.size() ||
        snapshot_id == parent->snapshot_id)
        reject("semantic append parent generation or shape changed");

    StageTimer timer;
    auto semantic = prepare_session_semantic_delta(
        std::move(session_binding), address_index, parent->edges.size(),
        parent->snapshot_id, std::move(parent_vrs_snapshot_id));
    timer.mark("source_bound_semantic_delta");

    std::vector<float> appended_strength(semantic.edge_rows.size(), .75F);
    auto canonical = prepare_canonical_vrs_append_delta(
        parent->edges, parent->strength, lineage, semantic.edge_rows,
        appended_strength, *edge_address_index);
    timer.mark("canonical_changed_groups");

    auto successor_terms = address_index->append_shared(semantic.appended_terms);
    auto successor_lineage = append_lineage(
        canonical.appended_member_group_ids, canonical.new_group_rows.size());
    timer.mark("shared_terms_and_membership");

    const auto old_group_count = parent->edges.size();
    auto new_edges = canonical.new_group_rows;
    std::vector<float> new_strength(new_edges.size());
    std::vector<std::uint8_t> initialized(new_edges.size(), 0);
    std::vector<std::size_t> changed;
    std::vector<float> base_values;
    std::vector<float> strength_values;
    changed.reserve(canonical.group_updates.size());
    base_values.reserve(canonical.group_updates.size());
    strength_values.reserve(canonical.group_updates.size());
    for (const auto& update : canonical.group_updates) {
        const auto stored = stored_float16_strength(update.current_strength);
        if (update.group < old_group_count) {
            changed.push_back(update.group);
            base_values.push_back(update.base_strength);
            strength_values.push_back(stored);
        } else {
            const auto offset = static_cast<std::size_t>(update.group) - old_group_count;
            if (offset >= new_edges.size() || initialized[offset])
                reject("canonical semantic group update changed");
            new_edges[offset].vrs_strength = update.base_strength;
            new_strength[offset] = stored;
            initialized[offset] = 1;
        }
    }
    if (std::ranges::find(initialized, std::uint8_t{0}) != initialized.end())
        reject("canonical semantic group update missing");

    std::vector<float> appended_nodes(semantic.appended_terms.size(), 0.0F);
    std::vector<std::uint8_t> appended_unresolved(semantic.appended_terms.size(), 0);
    auto candidate = prepare_event_delta(
        parent, std::move(snapshot_id), appended_nodes, appended_nodes,
        appended_unresolved, new_edges, new_strength,
        changed, base_values, changed, strength_values);
    timer.mark("sparse_numeric_inputs");

    auto successor_edges = edge_address_index->advance_event_delta(*parent, *candidate);
    timer.mark("canonical_address_extension");
    return SessionSemanticEventAppend(
        std::move(parent), std::move(candidate), std::move(successor_terms),
        std::move(successor_edges), std::move(successor_lineage),
        std::move(semantic), std::move(canonical), timer.take());
}

}  // namespace

SemanticEventAppend::SemanticEventAppend(
    std::shared_ptr<const EventSignalInputs> parent_value,
    std::shared_ptr<const EventSignalInputs> inputs_value,
    std::shared_ptr<const TermAddressIndex> address_index_value,
    std::shared_ptr<const CanonicalEdgeAddressIndex> edge_address_index_value,
    SparseCanonicalLineage lineage_value,
    SemanticGraphDelta semantic_delta_value,
    CanonicalVrsAppendDelta canonical_delta_value,
    std::vector<std::pair<std::string, std::uint64_t>> timings_ns_value)
    : parent(std::move(parent_value)), inputs(std::move(inputs_value)),
      address_index(std::move(address_index_value)),
      edge_address_index(std::move(edge_address_index_value)),
      lineage(std::move(lineage_value)),
      semantic_delta(std::move(semantic_delta_value)),
      canonical_delta(std::move(canonical_delta_value)),
      timings_ns(std::move(timings_ns_value)) {
    if (!parent || !inputs || !address_index || !edge_address_index ||
        inputs->delta_parent() != parent.get() ||
        address_index->size() != inputs->score.size() ||
        semantic_delta.address_index->size() + semantic_delta.appended_terms.size() !=
            address_index->size() ||
        parent->score.size() + semantic_delta.appended_terms.size() != inputs->score.size() ||
        canonical_delta.appended_member_group_ids.size() != semantic_delta.edge_rows.size() ||
        parent->edges.size() + canonical_delta.new_group_rows.size() != inputs->edges.size() ||
        lineage.group_count() != inputs->edges.size() ||
        lineage.member_count() < canonical_delta.appended_member_group_ids.size() ||
        timings_ns.size() != 5)
        reject("semantic event append candidate changed");
    edge_address_index->require_source(inputs->edges);
    static constexpr std::string_view names[] = {
        "source_bound_semantic_delta", "canonical_changed_groups",
        "shared_terms_and_membership", "sparse_numeric_inputs",
        "canonical_address_extension"};
    for (std::size_t index = 0; index < timings_ns.size(); ++index)
        if (timings_ns[index].first != names[index])
            reject("semantic event append timing stages changed");
}

SemanticEventAppendReceipt SemanticEventAppend::receipt() const {
    auto semantic_receipts = semantic_delta.receipts();
    std::vector<SemanticAppendProposalReceipt> proposals;
    proposals.reserve(semantic_receipts.size());
    for (auto& row : semantic_receipts) {
        if (row.member_edge_start < semantic_delta.edge_start)
            throw std::logic_error("semantic receipt edge start changed");
        const auto start = row.member_edge_start - semantic_delta.edge_start;
        if (start > canonical_delta.appended_member_group_ids.size() ||
            row.edge_roles.size() > canonical_delta.appended_member_group_ids.size() - start)
            throw std::logic_error("semantic canonical member binding changed");
        std::vector<std::uint32_t> canonical_ids(
            canonical_delta.appended_member_group_ids.begin() + start,
            canonical_delta.appended_member_group_ids.begin() +
                start + row.edge_roles.size());
        proposals.push_back({std::move(row), std::move(canonical_ids)});
    }
    std::uint64_t total = 0;
    for (const auto& [name, elapsed] : timings_ns) {
        (void)name;
        if (elapsed > std::numeric_limits<std::uint64_t>::max() - total)
            throw std::overflow_error("semantic stage time overflow");
        total += elapsed;
    }
    return {
        std::string(semantic_event_append_schema),
        parent->snapshot_id,
        inputs->snapshot_id,
        semantic_delta.appended_terms.size(),
        inputs->edges.size() - parent->edges.size(),
        canonical_delta.appended_member_group_ids.size(),
        canonical_delta.group_updates.size(),
        std::move(proposals),
        timings_ns,
        total,
    };
}

SessionSemanticEventAppend::SessionSemanticEventAppend(
    std::shared_ptr<const EventSignalInputs> parent_value,
    std::shared_ptr<const EventSignalInputs> inputs_value,
    std::shared_ptr<const TermAddressIndex> address_index_value,
    std::shared_ptr<const CanonicalEdgeAddressIndex> edge_address_index_value,
    SparseCanonicalLineage lineage_value,
    SessionSemanticGraphDelta semantic_delta_value,
    CanonicalVrsAppendDelta canonical_delta_value,
    std::vector<std::pair<std::string, std::uint64_t>> timings_ns_value)
    : parent(std::move(parent_value)), inputs(std::move(inputs_value)),
      address_index(std::move(address_index_value)),
      edge_address_index(std::move(edge_address_index_value)),
      lineage(std::move(lineage_value)),
      semantic_delta(std::move(semantic_delta_value)),
      canonical_delta(std::move(canonical_delta_value)),
      timings_ns(std::move(timings_ns_value)) {
    if (!parent || !inputs || !address_index || !edge_address_index ||
        inputs->delta_parent() != parent.get() ||
        address_index->size() != inputs->score.size() ||
        semantic_delta.address_index->size() + semantic_delta.appended_terms.size() !=
            address_index->size() ||
        parent->score.size() + semantic_delta.appended_terms.size() != inputs->score.size() ||
        canonical_delta.appended_member_group_ids.size() != semantic_delta.edge_rows.size() ||
        parent->edges.size() + canonical_delta.new_group_rows.size() != inputs->edges.size() ||
        lineage.group_count() != inputs->edges.size() ||
        lineage.member_count() < canonical_delta.appended_member_group_ids.size() ||
        timings_ns.size() != 5)
        reject("semantic event append candidate changed");
    edge_address_index->require_source(inputs->edges);
    static constexpr std::string_view names[] = {
        "source_bound_semantic_delta", "canonical_changed_groups",
        "shared_terms_and_membership", "sparse_numeric_inputs",
        "canonical_address_extension"};
    for (std::size_t index = 0; index < timings_ns.size(); ++index)
        if (timings_ns[index].first != names[index])
            reject("semantic event append timing stages changed");
}

SessionSemanticEventAppendReceipt SessionSemanticEventAppend::receipt() const {
    auto semantic_receipts = semantic_delta.receipts();
    std::vector<SessionSemanticAppendProposalReceipt> proposals;
    proposals.reserve(semantic_receipts.size());
    for (auto& row : semantic_receipts) {
        if (row.member_edge_start < semantic_delta.edge_start)
            throw std::logic_error("semantic receipt edge start changed");
        const auto start = row.member_edge_start - semantic_delta.edge_start;
        if (start > canonical_delta.appended_member_group_ids.size() ||
            row.member_edge_count > canonical_delta.appended_member_group_ids.size() - start)
            throw std::logic_error("semantic canonical member binding changed");
        std::vector<std::uint32_t> canonical_ids(
            canonical_delta.appended_member_group_ids.begin() + start,
            canonical_delta.appended_member_group_ids.begin() +
                start + row.member_edge_count);
        proposals.push_back({std::move(row), std::move(canonical_ids)});
    }
    std::uint64_t total = 0;
    for (const auto& [name, elapsed] : timings_ns) {
        (void)name;
        if (elapsed > std::numeric_limits<std::uint64_t>::max() - total)
            throw std::overflow_error("semantic stage time overflow");
        total += elapsed;
    }
    return {
        std::string(semantic_event_append_schema), parent->snapshot_id,
        inputs->snapshot_id, semantic_delta.appended_terms.size(),
        inputs->edges.size() - parent->edges.size(),
        canonical_delta.appended_member_group_ids.size(),
        canonical_delta.group_updates.size(), std::move(proposals),
        timings_ns, total,
    };
}

SemanticEventAppend prepare_semantic_event_append(
    std::shared_ptr<const EventSignalInputs> parent,
    std::shared_ptr<const TermAddressIndex> address_index,
    std::shared_ptr<const CanonicalVrsMemberLineage> lineage,
    std::shared_ptr<const CanonicalEdgeAddressIndex> edge_address_index,
    const std::vector<SemanticSourceEpisode>& episodes,
    const std::vector<SemanticEncoding>& proposals,
    std::string snapshot_id) {
    if (!lineage) reject("bound canonical lineage required");
    const auto dense_lineage = lineage;
    return prepare_impl(
        std::move(parent), std::move(address_index), *dense_lineage,
        [dense_lineage](
            const std::span<const std::uint32_t> groups,
            const std::uint64_t new_groups) {
            return SparseCanonicalLineage::append(dense_lineage, groups, new_groups);
        },
        std::move(edge_address_index), episodes, proposals, std::move(snapshot_id));
}

SessionSemanticEventAppend prepare_semantic_event_append(
    std::shared_ptr<const EventSignalInputs> parent,
    std::shared_ptr<const TermAddressIndex> address_index,
    std::shared_ptr<const CanonicalVrsMemberLineage> lineage,
    std::shared_ptr<const CanonicalEdgeAddressIndex> edge_address_index,
    std::shared_ptr<const BoundSessionSemantics> session_binding,
    std::string snapshot_id,
    std::string parent_vrs_snapshot_id) {
    if (!lineage) reject("bound canonical lineage required");
    const auto dense_lineage = lineage;
    return prepare_session_impl(
        std::move(parent), std::move(address_index), *dense_lineage,
        [dense_lineage](const std::span<const std::uint32_t> groups,
                        const std::uint64_t new_groups) {
            return SparseCanonicalLineage::append(dense_lineage, groups, new_groups);
        },
        std::move(edge_address_index), std::move(session_binding),
        std::move(snapshot_id), std::move(parent_vrs_snapshot_id));
}

SessionSemanticEventAppend prepare_semantic_event_append(
    std::shared_ptr<const EventSignalInputs> parent,
    std::shared_ptr<const TermAddressIndex> address_index,
    const SparseCanonicalLineage& lineage,
    std::shared_ptr<const CanonicalEdgeAddressIndex> edge_address_index,
    std::shared_ptr<const BoundSessionSemantics> session_binding,
    std::string snapshot_id,
    std::string parent_vrs_snapshot_id) {
    return prepare_session_impl(
        std::move(parent), std::move(address_index), lineage,
        [&lineage](const std::span<const std::uint32_t> groups,
                   const std::uint64_t new_groups) {
            return SparseCanonicalLineage::append(lineage, groups, new_groups);
        },
        std::move(edge_address_index), std::move(session_binding),
        std::move(snapshot_id), std::move(parent_vrs_snapshot_id));
}

SemanticEventAppend prepare_semantic_event_append(
    std::shared_ptr<const EventSignalInputs> parent,
    std::shared_ptr<const TermAddressIndex> address_index,
    const SparseCanonicalLineage& lineage,
    std::shared_ptr<const CanonicalEdgeAddressIndex> edge_address_index,
    const std::vector<SemanticSourceEpisode>& episodes,
    const std::vector<SemanticEncoding>& proposals,
    std::string snapshot_id) {
    return prepare_impl(
        std::move(parent), std::move(address_index), lineage,
        [&lineage](const std::span<const std::uint32_t> groups,
                   const std::uint64_t new_groups) {
            return SparseCanonicalLineage::append(lineage, groups, new_groups);
        },
        std::move(edge_address_index), episodes, proposals, std::move(snapshot_id));
}

}  // namespace swegca::world
