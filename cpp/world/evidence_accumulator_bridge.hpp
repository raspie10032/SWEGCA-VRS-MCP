#pragma once

#include "world/evidence_accumulator.hpp"
#include "world/synapse_arbiter.hpp"

#include <filesystem>
#include <string>
#include <string_view>

namespace swegca::world {

inline constexpr std::string_view evidence_accumulator_source_sha256 =
    "5495b2fbe03fc8c6c72ec13e23ed64269ed860d2cfc206d9f55dce1aa08647e5";

[[nodiscard]] SynapseProposal accumulator_gated_proposal(
    const WorldState& world, const SynapseProposal& proposal,
    const AccumulatorDecision& decision);
[[nodiscard]] SynapseProposal accumulator_gated_proposal(
    const CognitiveState& world, const SynapseProposal& proposal,
    const AccumulatorDecision& decision);

void append_accumulator_audit_record(
    const std::filesystem::path& path, const EvidenceObservation& observation,
    const AccumulatorUpdate& update, std::string world_hash_before,
    std::string world_hash_after, std::string accumulator_version,
    std::string arbiter_version);

}  // namespace swegca::world
