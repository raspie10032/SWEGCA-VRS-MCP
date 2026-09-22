#include "graph_auxiliary_generation.hpp"

#include "digest.hpp"

#include <cstddef>
#include <limits>
#include <stdexcept>
#include <utility>

namespace swegca::vrs {
namespace {

// SWEGCA: src/swegca_vrs2/store.py@c06092a:407-408
std::string chained_snapshot(std::string_view parent,
                             std::string_view event,
                             const Json::Array& ordered_rows) {
    const auto content = sha256_hex(Json(ordered_rows).canonical());
    Json::Array chain;
    chain.emplace_back(std::string(parent));
    chain.emplace_back(std::string(event));
    chain.emplace_back(content);
    return sha256_hex(Json(std::move(chain)).canonical());
}

// SWEGCA: src/swegca_vrs2/store.py@c06092a:407-408
Json::Array alias_rows(const std::map<std::string, std::string>& aliases) {
    Json::Array rows;
    rows.reserve(aliases.size());
    for (const auto& [alias, canonical] : aliases) {
        Json::Array pair;
        pair.emplace_back(alias);
        pair.emplace_back(canonical);
        rows.emplace_back(std::move(pair));
    }
    return rows;
}

// SWEGCA: src/swegca_vrs2/store.py@c06092a:418-419
Json::Array usage_rows(
    const std::map<std::string, std::array<std::int64_t, 2>>& usage) {
    Json::Array rows;
    rows.reserve(usage.size());
    for (const auto& [source, counts] : usage) {
        Json::Array pair;
        pair.emplace_back(source);
        Json::Array values;
        values.emplace_back(counts[0]);
        values.emplace_back(counts[1]);
        pair.emplace_back(std::move(values));
        rows.emplace_back(std::move(pair));
    }
    return rows;
}

// SWEGCA: src/swegca_vrs2/store.py@c06092a:408-419
void set_receipt(Json::Object& receipt, std::string_view status,
                 std::string_view parent_snapshot_id, std::string_view count_key,
                 std::size_t count) {
    if (count > static_cast<std::size_t>(
                    std::numeric_limits<std::int64_t>::max()))
        throw std::runtime_error("graph_auxiliary_count_invalid");
    receipt.insert_or_assign("status", Json(std::string(status)));
    receipt.insert_or_assign("parent_snapshot_id",
                             Json(std::string(parent_snapshot_id)));
    receipt.insert_or_assign(std::string(count_key),
                             Json(static_cast<std::int64_t>(count)));
}

}  // namespace

// SWEGCA: src/swegca_vrs2/store.py@c06092a:396-410
GraphAuxiliaryState graph_with_aliases(
    const GraphAuxiliaryState& current, std::string canonical,
    const std::vector<std::string>& aliases) {
    auto merged = current.aliases;
    if (const auto found = merged.find(canonical); found != merged.end())
        canonical = found->second;
    for (const auto& alias : aliases)
        if (alias != canonical) merged[alias] = canonical;
    const auto before_flatten = merged;
    for (const auto& [alias, initial] : before_flatten) {
        auto root = initial;
        std::size_t hops = 0;
        while (true) {
            const auto found = merged.find(root);
            if (found == merged.end() || found->second == root) break;
            root = found->second;
            if (++hops > merged.size())
                throw std::runtime_error("graph_alias_cycle");
        }
        merged[alias] = std::move(root);
    }
    GraphAuxiliaryState next = current;
    next.snapshot_id = chained_snapshot(current.snapshot_id, "alias",
                                        alias_rows(merged));
    next.aliases = std::move(merged);
    set_receipt(next.last_receipt, "alias", current.snapshot_id,
                "aliases", next.aliases.size());
    return next;
}

// SWEGCA: src/swegca_vrs2/store.py@c06092a:412-421
GraphAuxiliaryState graph_with_usage(
    const GraphAuxiliaryState& current,
    const std::map<std::string, std::array<std::int64_t, 2>>& counts) {
    GraphAuxiliaryState next = current;
    for (const auto& [source, pair] : counts) next.usage[source] = pair;
    next.snapshot_id = chained_snapshot(current.snapshot_id, "usage",
                                        usage_rows(next.usage));
    set_receipt(next.last_receipt, "usage", current.snapshot_id,
                "usage_sources", next.usage.size());
    return next;
}

// SWEGCA: src/swegca_vrs2/store.py@c06092a:917-933
GraphAuxiliaryState replay_graph_auxiliary_event(
    const GraphAuxiliaryState& current, const NativeJournalEntry& entry) {
    if (entry.kind == NativeJournalEntryKind::alias) {
        std::vector<std::string> aliases;
        for (const auto& value : entry.value.at("aliases").array())
            aliases.push_back(value.string());
        return graph_with_aliases(current,
                                  entry.value.at("canonical").string(), aliases);
    }
    if (entry.kind == NativeJournalEntryKind::usage) {
        std::map<std::string, std::array<std::int64_t, 2>> counts;
        for (const auto& [source, pair] : entry.value.at("counts").object()) {
            const auto& values = pair.array();
            if (values.size() != 2)
                throw std::runtime_error("stored_usage_counts_invalid");
            counts.emplace(source, std::array<std::int64_t, 2>{
                values[0].integer(), values[1].integer()});
        }
        return graph_with_usage(current, counts);
    }
    throw std::runtime_error("graph_auxiliary_event_kind_invalid");
}

// SWEGCA: src/swegca_vrs2/store.py@c06092a:396-421
std::shared_ptr<const ValidatedEventVrsInputs> prepare_graph_auxiliary_inputs(
    std::shared_ptr<const ValidatedEventVrsInputs> parent,
    const GraphAuxiliaryState& current,
    const GraphAuxiliaryState& successor) {
    if (!parent ||
        parent->require_validated_immutable().snapshot_id() != current.snapshot_id ||
        successor.last_receipt.at("parent_snapshot_id").string() !=
            current.snapshot_id)
        throw std::runtime_error("graph_auxiliary_parent_changed");
    return prepare_event_delta(std::move(parent), successor.snapshot_id,
                               EventDeltaChanges{});
}

}  // namespace swegca::vrs
