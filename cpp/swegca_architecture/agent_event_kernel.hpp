#pragma once
#include "swegca_architecture/session_kernel.hpp"

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
} // namespace swegca::architecture::kernel
