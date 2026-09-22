#include "graph_auxiliary_generation.hpp"

#include "digest.hpp"
#include "graph_append.hpp"
#include "memory_vrs_pair.hpp"
#include "unicode.hpp"

#include <cstddef>
#include <limits>
#include <set>
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

// SWEGCA: src/swegca_vrs2/store.py@c06092a:1326-1328
std::string strip_python_space(std::string_view text) {
    const auto points = decode_utf8(text);
    std::size_t first = 0;
    std::size_t last = points.size();
    while (first < last && python_space(points[first])) ++first;
    while (last > first && python_space(points[last - 1])) --last;
    std::string trimmed;
    for (auto at = first; at < last; ++at) append_utf8(trimmed, points[at]);
    return trimmed;
}

// SWEGCA: src/swegca_vrs2/store.py@c06092a:1331-1332
std::string first_python_characters(std::string_view text, std::size_t limit) {
    const auto points = decode_utf8(text);
    std::string prefix;
    for (std::size_t at = 0; at < points.size() && at < limit; ++at)
        append_utf8(prefix, points[at]);
    return prefix;
}

// SWEGCA: src/swegca_vrs2/store.py@c06092a:1333-1334
std::string alias_root(const GraphAuxiliaryState& state,
                       std::string_view proposition) {
    const auto found = state.aliases.find(std::string(proposition));
    return found == state.aliases.end() ? std::string(proposition) :
                                          found->second;
}

}  // namespace

// SWEGCA: src/swegca_vrs2/store.py@c06092a:383-386
GraphAuxiliaryState empty_graph_auxiliary_state(std::string_view identity) {
    return GraphAuxiliaryState{empty_graph_snapshot_id(identity), {}, {}, {}};
}

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

// SWEGCA: src/swegca_vrs2/store.py@c06092a:1318-1347
std::optional<GraphAliasUpdatePlan> plan_graph_alias_update(
    const HotIndexRead& memory, const GraphAuxiliaryState& current,
    std::string canonical, const std::vector<std::string>& aliases) {
    canonical = strip_python_space(canonical);
    std::set<std::string> distinct;
    for (const auto& alias : aliases) {
        auto cleaned = strip_python_space(alias);
        if (!cleaned.empty() && cleaned != canonical)
            distinct.insert(std::move(cleaned));
    }
    if (canonical.empty() || distinct.empty())
        throw std::runtime_error("alias_binding_empty");
    std::vector<std::string> prepared(distinct.begin(), distinct.end());
    std::vector<std::string> unknown;
    const auto check_known = [&](const std::string& proposition) {
        if (memory.proposition_ids(proposition).empty() &&
            !current.aliases.contains(proposition))
            unknown.push_back(proposition);
    };
    check_known(canonical);
    for (const auto& alias : prepared) check_known(alias);
    if (!unknown.empty()) {
        std::string joined;
        for (const auto& proposition : unknown) {
            if (!joined.empty()) joined += "; ";
            joined += proposition;
        }
        throw std::runtime_error("unknown_proposition: " +
                                 first_python_characters(joined, 300));
    }
    const auto target = alias_root(current, canonical);
    bool unchanged = true;
    for (const auto& alias : prepared) {
        const auto binding = current.aliases.find(alias);
        if (binding == current.aliases.end() || binding->second != target) {
            unchanged = false;
            break;
        }
    }
    if (unchanged) return std::nullopt;
    Json::Array alias_rows;
    alias_rows.reserve(prepared.size());
    for (const auto& alias : prepared) alias_rows.emplace_back(alias);
    Json::Object body;
    body.emplace("kind", Json(std::string("alias")));
    body.emplace("canonical", Json(canonical));
    body.emplace("aliases", Json(std::move(alias_rows)));
    Json journal_body(std::move(body));
    auto fingerprint = sha256_hex(journal_body.canonical());
    auto successor = graph_with_aliases(current, canonical, prepared);
    auto pair = full_current_pair_snapshot_id(memory.snapshot_id(),
                                              successor.snapshot_id);
    return GraphAliasUpdatePlan{
        std::move(journal_body), fingerprint,
        "alias:" + fingerprint.substr(0, 40),
        std::move(successor), std::move(pair)};
}

// SWEGCA: src/swegca_vrs2/store.py@c06092a:1291-1317
std::optional<GraphUsageUpdatePlan> plan_graph_usage_update(
    const PublishedHotIndex& memory, const GraphAuxiliaryState& current,
    const std::map<std::string, std::array<std::int64_t, 2>>& counts) {
    std::map<std::string, std::array<std::int64_t, 2>> known;
    for (const auto& [source, pair] : counts) {
        const auto prior = current.usage.find(source);
        if ((prior == current.usage.end() || prior->second != pair) &&
            memory.has_live_source(source))
            known.emplace(source, pair);
    }
    if (known.empty()) return std::nullopt;
    Json::Object count_rows;
    for (const auto& [source, pair] : known) {
        Json::Array values;
        values.emplace_back(pair[0]);
        values.emplace_back(pair[1]);
        count_rows.emplace(source, Json(std::move(values)));
    }
    Json::Object body;
    body.emplace("kind", Json(std::string("usage")));
    body.emplace("counts", Json(std::move(count_rows)));
    Json journal_body(std::move(body));
    auto fingerprint = sha256_hex(journal_body.canonical());
    auto successor = graph_with_usage(current, known);
    auto pair_snapshot_id = full_current_pair_snapshot_id(
        memory.snapshot_id(), successor.snapshot_id);
    return GraphUsageUpdatePlan{
        std::move(journal_body), fingerprint,
        "usage:" + fingerprint.substr(0, 40),
        std::move(successor), std::move(pair_snapshot_id)};
}

}  // namespace swegca::vrs
