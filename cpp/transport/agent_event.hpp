#pragma once
#include "transport/json.hpp"
#include "swegca_architecture/agent_event_kernel.hpp"
#include "swegca_architecture/sha256.hpp"
#include "swegca_architecture/agent_delivery_identity.hpp"
#include <array>
#include <cstdint>
#include <utility>
#include <optional>
#include <stdexcept>

namespace swegca::transport {
// Compute at session binding time, never insert this work ahead of each input.
// Instance is chosen by the local adapter owner, not by untrusted event JSON.
inline architecture::DigestBytes agent_session_identity(std::string_view provider,
    std::string_view instance,std::string_view session) {
    if(provider.empty()||instance.empty()||session.empty())throw std::invalid_argument("empty agent session identity");
    architecture::Sha256 hash;hash.update("SWEGCA agent session v1");
    for(auto part:{provider,instance,session}){
        std::array<std::byte,8> length{};
        for(unsigned n=0;n<8;++n)length[n]=std::byte((std::uint64_t(part.size())>>(n*8))&255);
        hash.update(length);hash.update(part);
    }
    return hash.finish();
}

// Same native bytes and original timestamp under the same sequence constitute
// the same delivery. Shuffle seed/step are processing controls, not new evidence.
using architecture::agent_delivery_identity;

// Owns the complete native envelope and its parsed fields. The event is not a
// recorded experience until the Main ingress stores it through VRS. Preserve
// original bytes, including unknown fields; do not reconstruct them from JSON.
class AgentEvent final {
public:
    AgentEvent(const AgentEvent&)=delete;
    AgentEvent& operator=(const AgentEvent&)=delete;
    AgentEvent(AgentEvent&&) noexcept=default;
    [[nodiscard]] architecture::kernel::AgentEventKind kind() const noexcept{return kind_;}
    [[nodiscard]] std::string_view native_bytes() const noexcept{return native_;}
    [[nodiscard]] std::string_view session() const{return parsed_.at("session_id").string();}
    [[nodiscard]] std::string_view native_name() const{return parsed_.at("hook_event_name").string();}
    [[nodiscard]] std::optional<std::string_view> prompt() const{
        if(kind_!=architecture::kernel::AgentEventKind::input)return std::nullopt;
        return parsed_.at("prompt").string();
    }
    [[nodiscard]] const Json& fields() const noexcept{return parsed_;}
private:
    friend AgentEvent adapt_codex_hook(std::string_view,std::pmr::memory_resource&);
    AgentEvent(std::string_view native,Json parsed,architecture::kernel::AgentEventKind kind,
        std::pmr::memory_resource& memory):native_(native,&memory),parsed_(std::move(parsed)),kind_(kind){}
    std::pmr::string native_;
    Json parsed_;
    architecture::kernel::AgentEventKind kind_;
};

// Codex's SessionEnd (reason=other), Stop, compaction and subagent completion
// never imply explicit VRS end. Hook parsing does not read transcript paths.
inline AgentEvent adapt_codex_hook(std::string_view bytes,std::pmr::memory_resource& memory){
    using architecture::kernel::AgentEventKind;
    auto parsed=parse_json(bytes,memory);
    if(parsed.kind!=Json::Kind::object)throw std::invalid_argument("hook event must be an object");
    const auto session=parsed.at("session_id").string();
    const auto name=parsed.at("hook_event_name").string();
    if(session.empty()||name.empty())throw std::invalid_argument("empty native event identity");
    auto kind=AgentEventKind::content;
    if(name=="UserPromptSubmit"){
        (void)parsed.at("prompt").string();kind=AgentEventKind::input;
    }else if(name=="SessionStart"||name=="SessionEnd"||name=="Stop"||name=="Interrupt"||
             name=="PreCompact"||name=="PostCompact"||name=="SubagentStart"||name=="SubagentStop"){
        kind=AgentEventKind::lifecycle;
    }
    return AgentEvent(bytes,std::move(parsed),kind,memory);
}
} // namespace swegca::transport
