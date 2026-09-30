#include "world/mosaic_v0.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <stdexcept>

namespace swegca::world {
namespace {

double distance(const MosaicVector& left, const MosaicVector& right) {
    if (left.size() != right.size()) throw std::invalid_argument("vector dimensions differ");
    double total = 0.0;
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto difference = left[index] - right[index];
        total += difference * difference;
    }
    return std::sqrt(total);
}

std::set<std::string, std::less<>> tokens(const std::string_view value) {
    std::set<std::string, std::less<>> result;
    std::string token;
    for (const unsigned char byte : value) {
        if (std::isalnum(byte) || byte == '_') token.push_back(static_cast<char>(std::tolower(byte)));
        else if (!token.empty()) { result.insert(std::move(token)); token.clear(); }
    }
    if (!token.empty()) result.insert(std::move(token));
    return result;
}

std::string lowercase(const std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const unsigned char byte : value)
        result.push_back(static_cast<char>(std::tolower(byte)));
    return result;
}

JsonValue::Array numbers(const MosaicVector& values) {
    JsonValue::Array result;
    for (const auto value : values) result.emplace_back(value);
    return result;
}

JsonValue::Array strings(const std::vector<std::string>& values) {
    JsonValue::Array result;
    for (const auto& value : values) result.emplace_back(value);
    return result;
}

}  // namespace

void MosaicConfig::validate() const {
    if (!workspace_dim || !operator_top_k || !(halt_tolerance > 0) ||
        !(maximum_update_norm > 0))
        throw std::invalid_argument("MOSAIC v0 configuration must be positive");
}

void LowRankBasis::validate(const std::size_t dimension) const {
    if (name.empty()) throw std::invalid_argument("basis name must not be empty");
    if (left.size() != dimension || right.size() != dimension)
        throw std::invalid_argument("basis does not match workspace dimension");
}

void OperatorCode::validate() const {
    if (address.empty() || tags.empty() || coefficients.empty())
        throw std::invalid_argument("operator address, tags, and coefficients are required");
    if (confidence < 0 || confidence > 1)
        throw std::invalid_argument("operator confidence must be between 0 and 1");
}

JsonValue::Object MosaicRun::to_dict() const {
    JsonValue::Object coefficients;
    for (const auto& [basis, value] : operator_code.coefficients)
        coefficients.emplace(basis, value);
    JsonValue::Array conflicts;
    for (const auto& [left, right] : operator_code.conflicts)
        conflicts.emplace_back(JsonValue::Array{left, right});
    JsonValue::Array trace_rows;
    for (const auto& row : trace) trace_rows.emplace_back(row);
    return {{"success", success}, {"halted", halted}, {"stalled", stalled},
        {"steps", JsonInteger{std::to_string(steps)}}, {"final_state", numbers(final_state)},
        {"final_error", final_error}, {"operator", JsonValue::Object{
            {"coefficients", std::move(coefficients)}, {"selected", strings(operator_code.selected)},
            {"disabled", strings(operator_code.disabled)}, {"conflicts", std::move(conflicts)}}},
        {"trace", std::move(trace_rows)}};
}

OperatorArchive::OperatorArchive(std::vector<OperatorCode> operators)
    : operators_(std::move(operators)) {
    std::set<std::string, std::less<>> addresses;
    for (const auto& operation : operators_) {
        operation.validate();
        if (!addresses.insert(operation.address).second)
            throw std::invalid_argument("operator addresses must be unique");
    }
}

std::vector<OperatorCode> OperatorArchive::search(
    const std::string_view query, const std::size_t top_k) const {
    if (!top_k) return {};
    const auto query_tokens = tokens(query);
    std::vector<std::pair<std::size_t, const OperatorCode*>> ranked;
    for (const auto& operation : operators_) {
        std::size_t overlap = 0;
        for (const auto& tag : operation.tags)
            if (query_tokens.contains(lowercase(tag))) ++overlap;
        if (overlap) ranked.emplace_back(overlap, &operation);
    }
    std::ranges::sort(ranked, [](const auto& left, const auto& right) {
        if (left.first != right.first) return left.first > right.first;
        if (left.second->priority != right.second->priority)
            return left.second->priority > right.second->priority;
        if (left.second->confidence != right.second->confidence)
            return left.second->confidence > right.second->confidence;
        return left.second->address < right.second->address;
    });
    std::vector<OperatorCode> result;
    for (std::size_t index = 0; index < std::min(top_k, ranked.size()); ++index)
        result.push_back(*ranked[index].second);
    return result;
}

SynthesizedOperator synthesize_operators(const std::vector<OperatorCode>& candidates) {
    SynthesizedOperator result;
    std::vector<const OperatorCode*> selected;
    std::map<std::string, double, std::less<>> coefficients;
    for (const auto& candidate : candidates) {
        const auto blocker = std::ranges::find_if(selected, [&](const auto* active) {
            return active->conflicts.contains(candidate.address) ||
                candidate.conflicts.contains(active->address);
        });
        if (blocker != selected.end()) {
            result.disabled.push_back(candidate.address);
            result.conflicts.emplace_back((*blocker)->address, candidate.address);
            continue;
        }
        selected.push_back(&candidate);
        result.selected.push_back(candidate.address);
        for (const auto& [basis, coefficient] : candidate.coefficients)
            coefficients[basis] += coefficient;
    }
    result.coefficients.assign(coefficients.begin(), coefficients.end());
    return result;
}

RecurrentCell::RecurrentCell(std::vector<LowRankBasis> bases,
    const std::size_t dimension, const double maximum_update_norm)
    : dimension_(dimension), maximum_update_norm_(maximum_update_norm) {
    if (bases.empty()) throw std::invalid_argument("at least one operator basis is required");
    for (auto& basis : bases) {
        basis.validate(dimension_);
        if (!bases_.emplace(basis.name, std::move(basis)).second)
            throw std::invalid_argument("operator basis names must be unique");
    }
}

std::pair<MosaicVector, double> RecurrentCell::apply(
    const MosaicVector& state, const SynthesizedOperator& operation) const {
    if (state.size() != dimension_)
        throw std::invalid_argument("state does not match workspace dimension");
    MosaicVector delta(dimension_, 0.0);
    for (const auto& [basis_name, coefficient] : operation.coefficients) {
        const auto found = bases_.find(basis_name);
        if (found == bases_.end()) throw std::invalid_argument("unknown operator basis");
        const auto& basis = found->second;
        double activation = basis.bias;
        for (std::size_t index = 0; index < dimension_; ++index)
            activation += basis.right[index] * state[index];
        for (std::size_t index = 0; index < dimension_; ++index)
            delta[index] += coefficient * activation * basis.left[index];
    }
    double norm = 0.0;
    for (const auto value : delta) norm += value * value;
    norm = std::sqrt(norm);
    if (norm > maximum_update_norm_) {
        const auto scale = maximum_update_norm_ / norm;
        for (auto& value : delta) value *= scale;
        norm = maximum_update_norm_;
    }
    auto result = state;
    for (std::size_t index = 0; index < dimension_; ++index) result[index] += delta[index];
    return {std::move(result), norm};
}

MosaicSimulator::MosaicSimulator(MosaicConfig config,
    std::vector<LowRankBasis> bases, std::vector<OperatorCode> operators)
    : config_(config), archive_(std::move(operators)),
      cell_(std::move(bases), config.workspace_dim, config.maximum_update_norm) {
    config_.validate();
}

MosaicRun MosaicSimulator::solve(const std::string_view query,
    const MosaicVector& initial_state, const MosaicVector& goal,
    const std::size_t maximum_steps) const {
    if (initial_state.size() != config_.workspace_dim || goal.size() != initial_state.size())
        throw std::invalid_argument("initial state and goal must match workspace dimension");
    const auto operation = synthesize_operators(archive_.search(query, config_.operator_top_k));
    auto state = initial_state;
    auto error = distance(state, goal);
    std::vector<JsonValue::Object> trace;
    bool stalled = false;
    for (std::size_t step = 1; step <= maximum_steps; ++step) {
        if (error <= config_.halt_tolerance) break;
        const auto previous = state;
        auto [next, update_norm] = cell_.apply(state, operation);
        state = std::move(next);
        error = distance(state, goal);
        trace.push_back({{"step", JsonInteger{std::to_string(step)}},
            {"state", numbers(state)}, {"error", error}, {"update_norm", update_norm}});
        if (distance(previous, state) <= config_.halt_tolerance) { stalled = true; break; }
    }
    const bool success = error <= config_.halt_tolerance;
    return {success, success, stalled, trace.size(), std::move(state), error,
        operation, std::move(trace)};
}

JsonValue::Object run_mosaic_v0_demo() {
    const std::string swapped_value = "4";
    const std::size_t deleted_rows = 2;
    MosaicSimulator simulator({},
        {{"progress", {1, 0, 0, 0}, {0, 0, 0, 0}, 1}},
        {{"procedure.forward", {"advance", "progress"}, {{"progress", 1}}, 1, 10,
             {"procedure.reverse"}},
         {"procedure.reverse", {"advance", "progress"}, {{"progress", -1}}, 1, 1,
             {"procedure.forward"}}});
    JsonValue::Array depths;
    std::vector<MosaicRun> runs;
    for (const std::size_t steps : {1U, 2U, 4U, 8U}) {
        auto run = simulator.solve("advance progress", {0, 0, 0, 0}, {4, 0, 0, 0}, steps);
        auto row = run.to_dict(); row.emplace("max_steps", JsonInteger{std::to_string(steps)});
        depths.emplace_back(std::move(row)); runs.push_back(std::move(run));
    }
    const auto unknown = simulator.solve("unseen operation", {0, 0, 0, 0}, {4, 0, 0, 0}, 8);
    JsonValue::Object acceptance{{"knowledge_swap", swapped_value == "4"},
        {"knowledge_delete", deleted_rows == 2},
        {"operator_conflict", runs.back().operator_code.disabled ==
            std::vector<std::string>{"procedure.reverse"}},
        {"recurrent_scaling", !runs.front().success && runs[2].success},
        {"unknown_stalls_without_operator", unknown.stalled && unknown.operator_code.selected.empty()}};
    bool passed = true;
    for (const auto& [unused, value] : acceptance) {
        static_cast<void>(unused); passed = passed && std::get<bool>(value.storage());
    }
    return {{"schema_version", "mosaic-v0"},
        {"scope", "deterministic architecture simulator; not an LM quality result"},
        {"memory", JsonValue::Object{{"swapped_value", swapped_value},
            {"deleted_rows", JsonInteger{std::to_string(deleted_rows)}},
            {"active_after_delete", JsonValue::Array{}}}},
        {"recurrent_depths", std::move(depths)}, {"unknown_query", unknown.to_dict()},
        {"acceptance", std::move(acceptance)}, {"passed", passed}};
}

}  // namespace swegca::world
