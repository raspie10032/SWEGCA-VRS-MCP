#include "connectivity_regions.hpp"

#include "digest.hpp"
#include "python_fsum.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace swegca::vrs {
namespace {

struct WeightedCsr {
    std::vector<std::uint64_t> offsets;
    std::vector<std::uint32_t> neighbors;
    std::vector<double> weights;
};

struct WeightedEntry {
    std::uint32_t row;
    std::uint32_t column;
    double weight;
};

struct LocalMoves {
    std::vector<std::uint32_t> labels;
    bool converged;
    std::uint32_t sweeps;
};

struct Communities {
    std::vector<std::uint32_t> labels;
    bool converged;
    std::vector<std::uint32_t> sweeps;
};

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_connectivity_regions.py@7536139:162-164
bool generation_digest(std::string_view identifier) {
    return identifier.size() == 64 &&
        std::all_of(identifier.begin(), identifier.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        });
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_connectivity_regions.py@7536139:27-37
WeightedCsr csr(std::vector<WeightedEntry> entries, std::size_t size) {
    std::stable_sort(entries.begin(), entries.end(),
        [](const auto& left, const auto& right) {
            if (left.row != right.row) return left.row < right.row;
            if (left.column != right.column) return left.column < right.column;
            return left.weight < right.weight;
        });
    WeightedCsr result;
    result.offsets.assign(size + 1, 0);
    for (std::size_t at = 0; at < entries.size();) {
        const auto row = entries[at].row;
        const auto column = entries[at].column;
        if (row >= size || column >= size)
            throw std::runtime_error("invalid region endpoint");
        double total = 0;
        do {
            total += entries[at].weight;
            ++at;
        } while (at < entries.size() && entries[at].row == row &&
                 entries[at].column == column);
        result.neighbors.push_back(column);
        result.weights.push_back(total);
        ++result.offsets[static_cast<std::size_t>(row) + 1];
    }
    for (std::size_t at = 1; at < result.offsets.size(); ++at)
        result.offsets[at] += result.offsets[at - 1];
    return result;
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_connectivity_regions.py@7536139:40-44
std::vector<std::uint32_t> canonical(const std::vector<std::uint32_t>& labels) {
    std::map<std::uint32_t, std::uint32_t> remap;
    std::vector<std::uint32_t> result;
    result.reserve(labels.size());
    for (const auto label : labels) {
        const auto found = remap.emplace(label,
            static_cast<std::uint32_t>(remap.size())).first;
        result.push_back(found->second);
    }
    return result;
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_connectivity_regions.py@7536139:47-78
LocalMoves local_moves(const WeightedCsr& graph, std::uint32_t maximum_sweeps) {
    const auto size = graph.offsets.size() - 1;
    std::vector<double> degree(size, 0);
    for (std::size_t node = 0; node < size; ++node)
        for (auto at = graph.offsets[node]; at < graph.offsets[node + 1]; ++at)
            degree[node] += graph.weights[at];
    double mass = 0;
    for (const auto value : degree) mass += value;
    std::vector<std::uint32_t> labels(size);
    std::iota(labels.begin(), labels.end(), 0);
    auto totals = degree;
    if (mass == 0) return {std::move(labels), true, 0};
    if (!std::isfinite(mass)) throw std::runtime_error("association mass overflow");
    for (std::uint32_t sweep = 0; sweep < maximum_sweeps; ++sweep) {
        bool moved = false;
        for (std::size_t node = 0; node < size; ++node) {
            if (degree[node] == 0) continue;
            std::map<std::uint32_t, double> into;
            for (auto at = graph.offsets[node]; at < graph.offsets[node + 1]; ++at) {
                const auto neighbor = graph.neighbors[at];
                if (neighbor != node) into[labels[neighbor]] += graph.weights[at];
            }
            const auto old = labels[node];
            totals[old] -= degree[node];
            const auto score = [&](std::uint32_t group) {
                const auto found = into.find(group);
                const auto weight = found == into.end() ? 0.0 : found->second;
                return weight - degree[node] * totals[group] / mass;
            };
            auto best = old;
            auto best_score = score(old);
            const auto tolerance = 64.0 * std::numeric_limits<double>::epsilon() *
                                   std::max(mass, 1.0);
            for (const auto& [group, weight] : into) {
                (void)weight;
                const auto candidate = score(group);
                if (candidate > best_score + tolerance) {
                    best = group;
                    best_score = candidate;
                }
            }
            totals[best] += degree[node];
            labels[node] = best;
            moved |= best != old;
        }
        if (!moved) return {canonical(labels), true, sweep + 1};
    }
    return {canonical(labels), false, maximum_sweeps};
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_connectivity_regions.py@7536139:81-96
Communities communities(WeightedCsr graph, std::uint32_t maximum_sweeps,
                        std::uint32_t maximum_levels) {
    std::vector<std::uint32_t> labels(graph.offsets.size() - 1);
    std::iota(labels.begin(), labels.end(), 0);
    std::vector<std::uint32_t> sweeps;
    for (std::uint32_t level = 0; level < maximum_levels; ++level) {
        const auto local = local_moves(graph, maximum_sweeps);
        sweeps.push_back(local.sweeps);
        for (auto& label : labels) label = local.labels[label];
        labels = canonical(labels);
        if (!local.converged) return {std::move(labels), false, std::move(sweeps)};
        const auto next_size = local.labels.empty() ? 0 :
            *std::max_element(local.labels.begin(), local.labels.end()) + 1;
        if (next_size == local.labels.size())
            return {std::move(labels), true, std::move(sweeps)};
        std::vector<WeightedEntry> collapsed;
        collapsed.reserve(graph.neighbors.size());
        for (std::size_t row = 0; row < local.labels.size(); ++row)
            for (auto at = graph.offsets[row]; at < graph.offsets[row + 1]; ++at)
                collapsed.push_back(WeightedEntry{
                    local.labels[row], local.labels[graph.neighbors[at]],
                    graph.weights[at]});
        graph = csr(std::move(collapsed), next_size);
    }
    return {std::move(labels), false, std::move(sweeps)};
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_connectivity_regions.py@7536139:99-118
void memberships(const WeightedCsr& graph, const std::vector<std::uint32_t>& core,
                 std::vector<std::uint64_t>& offsets,
                 std::vector<std::uint32_t>& regions,
                 std::vector<double>& coefficients) {
    offsets.assign(core.size() + 1, 0);
    for (std::size_t node = 0; node < core.size(); ++node) {
        std::map<std::uint32_t, double> masses;
        for (auto at = graph.offsets[node]; at < graph.offsets[node + 1]; ++at)
            masses[core[graph.neighbors[at]]] += graph.weights[at];
        for (auto at = masses.begin(); at != masses.end();) {
            if (at->second <= 0) at = masses.erase(at);
            else ++at;
        }
        if (masses.empty()) masses[core[node]] = 1.0;
        PythonFsum sum;
        for (const auto& [group, mass] : masses) {
            (void)group;
            sum.add(mass);
        }
        const auto total = sum.finish();
        for (const auto& [group, mass] : masses) {
            regions.push_back(group);
            coefficients.push_back(mass / total);
        }
        offsets[node + 1] = regions.size();
    }
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_region_arrays.py@7536139:29-36
void reverse_memberships(const std::vector<std::uint64_t>& member_offsets,
                         const std::vector<std::uint32_t>& member_regions,
                         std::size_t region_count,
                         std::vector<std::uint64_t>& region_offsets,
                         std::vector<std::uint32_t>& region_nodes) {
    region_offsets.assign(region_count + 1, 0);
    for (const auto group : member_regions) ++region_offsets[group + 1];
    for (std::size_t at = 1; at < region_offsets.size(); ++at)
        region_offsets[at] += region_offsets[at - 1];
    region_nodes.resize(member_regions.size());
    auto cursor = region_offsets;
    for (std::size_t node = 0; node + 1 < member_offsets.size(); ++node)
        for (auto at = member_offsets[node]; at < member_offsets[node + 1]; ++at)
            region_nodes[cursor[member_regions[at]]++] = static_cast<std::uint32_t>(node);
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_connectivity_regions.py@7536139:192-196
std::string topology_digest(std::string_view snapshot,
                            const std::vector<std::uint32_t>& core,
                            std::uint32_t maximum_sweeps,
                            std::uint32_t maximum_levels, bool converged) {
    Sha256 digest;
    constexpr char prefix[] = "vrs-connectivity-modularity-overlap-v1";
    digest.update(std::string_view(prefix, sizeof(prefix)));
    digest.update(snapshot);
    for (const auto label : core) {
        std::array<std::byte, 8> encoded{};
        for (std::size_t at = 0; at < encoded.size(); ++at)
            encoded[at] = static_cast<std::byte>((static_cast<std::uint64_t>(label) >> (8 * at)) & 0xff);
        digest.update(encoded);
    }
    digest.update("(" + std::to_string(maximum_sweeps) + ", " +
                  std::to_string(maximum_levels) + ", " +
                  (converged ? "True" : "False") + ")");
    const auto bytes = digest.finish();
    constexpr char alphabet[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(64);
    for (const auto byte : bytes) {
        const auto value = std::to_integer<unsigned char>(byte);
        hex.push_back(alphabet[value >> 4]);
        hex.push_back(alphabet[value & 15]);
    }
    return hex;
}

}  // namespace

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_connectivity_regions.py@7536139:159-199
ConnectivityRegions ConnectivityRegions::build(
    std::shared_ptr<const AffectedGraphComponent> source,
    std::string vrs_snapshot_id, std::uint32_t maximum_sweeps,
    std::uint32_t maximum_levels) {
    if (!source || !generation_digest(vrs_snapshot_id))
        throw std::runtime_error("current VRS content digest required");
    if (maximum_sweeps == 0 || maximum_levels == 0)
        throw std::runtime_error("positive explicit work limits required");
    const auto size = source->nodes.size();
    const auto edge_count = source->local_source.size();
    if (source->local_target.size() != edge_count ||
        source->signs.size() != edge_count || source->strengths.size() != edge_count)
        throw std::runtime_error("edge array shapes differ");
    std::vector<WeightedEntry> associations;
    associations.reserve(edge_count * 2);
    for (std::size_t at = 0; at < edge_count; ++at) {
        const auto u = source->local_source[at], v = source->local_target[at];
        const auto weight = source->strengths[at];
        if (u >= size || v >= size || !std::isfinite(weight) || weight < 0)
            throw std::runtime_error("invalid VRS endpoint, sign or strength");
        associations.push_back(WeightedEntry{u, v, weight});
        associations.push_back(WeightedEntry{v, u, weight});
    }
    const auto graph = csr(std::move(associations), size);
    double association_mass = 0;
    for (const auto weight : graph.weights) association_mass += weight;
    if (!std::isfinite(association_mass))
        throw std::runtime_error("association mass overflow");
    auto found = communities(graph, maximum_sweeps, maximum_levels);
    ConnectivityRegions result;
    result.vrs_snapshot_id_ = std::move(vrs_snapshot_id);
    result.source_ = std::move(source);
    result.terms_ = result.source_->nodes;
    result.core_labels_ = std::move(found.labels);
    result.edge_source_ = result.source_->local_source;
    result.edge_target_ = result.source_->local_target;
    result.edge_sign_ = result.source_->signs;
    result.strengths_ = result.source_->strengths;
    result.converged_ = found.converged;
    result.sweeps_ = std::move(found.sweeps);
    memberships(graph, result.core_labels_, result.member_offsets_,
                result.member_regions_, result.member_weights_);
    const std::size_t region_count = result.core_labels_.empty() ? 0 :
        *std::max_element(result.core_labels_.begin(), result.core_labels_.end()) + 1;
    reverse_memberships(result.member_offsets_, result.member_regions_, region_count,
                        result.region_offsets_, result.region_nodes_);
    result.topology_id_ = topology_digest(result.vrs_snapshot_id_, result.core_labels_,
                                          maximum_sweeps, maximum_levels, result.converged_);
    return result;
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_connectivity_regions.py@7536139:201-203
void ConnectivityRegions::require_source(const AffectedGraphComponent& source,
                                          const std::string& vrs_snapshot_id) const {
    if (source_.get() != &source || vrs_snapshot_id_ != vrs_snapshot_id)
        throw std::runtime_error("region topology belongs to a different VRS generation");
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_connectivity_regions.py@7536139:205-211
std::vector<std::pair<std::uint32_t, double>> ConnectivityRegions::memberships_for_term(
    std::uint32_t local_node) const {
    if (local_node >= terms_.size()) throw std::out_of_range("region node outside directory");
    std::vector<std::pair<std::uint32_t, double>> result;
    for (auto at = member_offsets_[local_node]; at < member_offsets_[local_node + 1]; ++at)
        result.emplace_back(member_regions_[at], member_weights_[at]);
    return result;
}

// SWEGCA: src/swegca_vrs2/engine/mosaic_vrs_connectivity_regions.py@7536139:275-281
Json ConnectivityRegions::receipt() const {
    Json::Object result;
    result.emplace("topology_id", Json(topology_id_));
    result.emplace("vrs_snapshot_id", Json(vrs_snapshot_id_));
    result.emplace("region_count", Json(static_cast<std::int64_t>(region_offsets_.size() - 1)));
    result.emplace("term_count", Json(static_cast<std::int64_t>(terms_.size())));
    result.emplace("edge_count", Json(static_cast<std::int64_t>(edge_source_.size())));
    result.emplace("converged", Json(converged_));
    Json::Array sweeps;
    for (const auto count : sweeps_) sweeps.emplace_back(static_cast<std::int64_t>(count));
    result.emplace("sweeps", Json(std::move(sweeps)));
    result.emplace("membership_is_truth", Json(false));
    result.emplace("grants_authority", Json(false));
    result.emplace("full_memory_access_restricted", Json(false));
    result.emplace("original_experiences_split", Json(std::int64_t{0}));
    return Json(std::move(result));
}

}  // namespace swegca::vrs
