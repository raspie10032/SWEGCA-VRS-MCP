#pragma once

#include "world/cognitive_state.hpp"

#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace swegca::world {

inline constexpr std::string_view mosaic_v0_source_sha256 =
    "70ca9dbf4232a30b0310c6f39939b337d214d3f4fc77960cfb9f917a8e89d28f";
using MosaicVector = std::vector<double>;

struct MosaicConfig final {
    std::size_t workspace_dim{4};
    std::size_t operator_top_k{4};
    double halt_tolerance{1e-6};
    double maximum_update_norm{1.0};
    void validate() const;
};

struct LowRankBasis final {
    std::string name;
    MosaicVector left;
    MosaicVector right;
    double bias{};
    void validate(std::size_t dimension) const;
};

struct OperatorCode final {
    std::string address;
    std::set<std::string, std::less<>> tags;
    std::vector<std::pair<std::string, double>> coefficients;
    double confidence{1.0};
    std::int64_t priority{};
    std::set<std::string, std::less<>> conflicts;
    void validate() const;
};

struct SynthesizedOperator final {
    std::vector<std::pair<std::string, double>> coefficients;
    std::vector<std::string> selected;
    std::vector<std::string> disabled;
    std::vector<std::pair<std::string, std::string>> conflicts;
};

struct MosaicRun final {
    bool success{};
    bool halted{};
    bool stalled{};
    std::size_t steps{};
    MosaicVector final_state;
    double final_error{};
    SynthesizedOperator operator_code;
    std::vector<JsonValue::Object> trace;
    [[nodiscard]] JsonValue::Object to_dict() const;
};

class OperatorArchive final {
public:
    explicit OperatorArchive(std::vector<OperatorCode> operators);
    [[nodiscard]] std::vector<OperatorCode> search(
        std::string_view query, std::size_t top_k) const;
private:
    std::vector<OperatorCode> operators_;
};

[[nodiscard]] SynthesizedOperator synthesize_operators(
    const std::vector<OperatorCode>& candidates);

class RecurrentCell final {
public:
    RecurrentCell(std::vector<LowRankBasis> bases, std::size_t dimension,
        double maximum_update_norm);
    [[nodiscard]] std::pair<MosaicVector, double> apply(
        const MosaicVector& state, const SynthesizedOperator& operation) const;
private:
    std::map<std::string, LowRankBasis, std::less<>> bases_;
    std::size_t dimension_{};
    double maximum_update_norm_{};
};

class MosaicSimulator final {
public:
    MosaicSimulator(MosaicConfig config, std::vector<LowRankBasis> bases,
        std::vector<OperatorCode> operators);
    [[nodiscard]] MosaicRun solve(std::string_view query,
        const MosaicVector& initial_state, const MosaicVector& goal,
        std::size_t maximum_steps) const;
private:
    MosaicConfig config_;
    OperatorArchive archive_;
    RecurrentCell cell_;
};

[[nodiscard]] JsonValue::Object run_mosaic_v0_demo();

}  // namespace swegca::world
