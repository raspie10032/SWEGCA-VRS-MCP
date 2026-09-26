#pragma once
#include "swegca_architecture/session_kernel.hpp"
#include <cstdint>
#include <limits>

namespace swegca::architecture::kernel {
// Native agent adapters supply event facts, never evidence verdicts. A native
// lifecycle notification is not the host's explicit VRS close operation.
enum class AgentEventKind { input, content, lifecycle, explicit_end };
enum class AgentEventRoute { invalid, recall_then_record, record, end };
[[nodiscard]] constexpr AgentEventRoute route_agent_event(SessionPhase phase,AgentEventKind kind) noexcept {
    SessionPhase next;
    if(kind==AgentEventKind::explicit_end)
        return next_session_phase(phase,SessionOperation::end,next)?AgentEventRoute::end:AgentEventRoute::invalid;
    if(!next_session_phase(phase,SessionOperation::append,next))return AgentEventRoute::invalid;
    switch(kind){
    case AgentEventKind::input:return AgentEventRoute::recall_then_record;
    case AgentEventKind::content:case AgentEventKind::lifecycle:return AgentEventRoute::record;
    default:return AgentEventRoute::invalid;
    }
}
// Delivery identity is transport provenance, never evidence support. A replayed
// delivery acknowledges the committed original without a second refinement.
enum class AgentDeliveryRoute { append, reuse, reject };
[[nodiscard]] constexpr AgentDeliveryRoute route_agent_delivery(SessionPhase phase,
    bool found,bool identical,bool has_previous,std::uint64_t previous,std::uint64_t incoming) noexcept {
    SessionPhase next;
    if(!next_session_phase(phase,SessionOperation::append,next))return AgentDeliveryRoute::reject;
    if(found)return identical?AgentDeliveryRoute::reuse:AgentDeliveryRoute::reject;
    if(!has_previous)return incoming==0?AgentDeliveryRoute::append:AgentDeliveryRoute::reject;
    return previous!=std::numeric_limits<std::uint64_t>::max() && incoming==previous+1
        ?AgentDeliveryRoute::append:AgentDeliveryRoute::reject;
}
} // namespace swegca::architecture::kernel
