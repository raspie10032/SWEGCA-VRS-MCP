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
    [[nodiscard]] std::string_view session() const{
        if(!bound_session_.empty())return bound_session_;
        if(!app_server_)return parsed_.at("session_id").string();
        const auto& params=parsed_.at("params");
        return parsed_.at("method").string()=="thread/started"?params.at("thread").at("id").string():params.at("threadId").string();
    }
    [[nodiscard]] std::string_view native_name() const{if(app_server_&&!parsed_.find("method"))return {};return parsed_.at(app_server_?"method":"hook_event_name").string();}
    [[nodiscard]] bool is_app_server() const noexcept{return app_server_;}
    [[nodiscard]] std::optional<std::string_view> prompt() const{
        if(app_server_||kind_!=architecture::kernel::AgentEventKind::input)return std::nullopt;
        return parsed_.at("prompt").string();
    }
    [[nodiscard]] std::string_view cue_media() const noexcept{
        return app_server_?"application/vnd.swegca.codex-input-v1":"text/plain";
    }
    [[nodiscard]] std::string_view cue_content() const{
        if(kind_!=architecture::kernel::AgentEventKind::input)throw std::invalid_argument("event has no input cue");
        if(!app_server_)return *prompt();
        // The exclusive event owner materializes the exact same encoded cue
        // only when VRS consumes it. Transport-only owners never need a copy.
        // An input array encodes to at least "[]"; empty means not built yet.
        if(cue_.empty())cue_=encode_json(parsed_.at("params").at("input"),*cue_.get_allocator().resource());
        return cue_;
    }
    [[nodiscard]] const Json& fields() const noexcept{return parsed_;}
private:
    friend class AppServerRequests;
    friend class AppServerWire;
    static AgentEvent from_app_server(std::string_view,Json,std::pmr::memory_resource&);
    static AgentEvent from_app_server_connection(std::string_view,Json,std::string_view,std::pmr::memory_resource&);
    friend AgentEvent adapt_codex_app_server_connection(std::string_view,std::string_view,std::pmr::memory_resource&);
    friend AgentEvent adapt_codex_app_server(std::string_view,std::pmr::memory_resource&);
    friend AgentEvent adapt_owned_codex_app_server(std::pmr::string,std::pmr::memory_resource&,std::string_view);
    friend AgentEvent adapt_codex_hook(std::string_view,std::pmr::memory_resource&);
    AgentEvent(std::string_view native,Json parsed,architecture::kernel::AgentEventKind kind,
        std::pmr::memory_resource& memory):native_(native,&memory),parsed_(std::move(parsed)),kind_(kind),cue_(&memory),bound_session_(&memory){}
    std::pmr::string native_;
    Json parsed_;
    architecture::kernel::AgentEventKind kind_;
    bool app_server_=false;
    mutable std::pmr::string cue_;
    std::pmr::string bound_session_;
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
// Parse actual app-server request/notification envelopes. Replies without an
// explicit thread binding need the owning transport's request-ID correlation;
// they are rejected here rather than assigned to whichever session is selected.
inline AgentEvent AgentEvent::from_app_server(std::string_view bytes,Json parsed,std::pmr::memory_resource& memory){
    using architecture::kernel::AgentEventKind;
    const auto method=parsed.at("method").string();
    const auto& params=parsed.at("params");
    const bool started=method=="thread/started",resumed=method=="thread/resume";
    const auto session=started?params.at("thread").at("id").string():params.at("threadId").string();
    if(started){
        if(parsed.find("id"))throw std::invalid_argument("thread started must be a notification");
        if(const auto* explicit_id=params.find("threadId");explicit_id&&explicit_id->string()!=session)
            throw std::invalid_argument("conflicting started thread identities");
    }
    if(method.empty()||session.empty())throw std::invalid_argument("empty app-server identity");
    if(resumed){
        const auto& id=parsed.at("id");
        if(id.kind!=Json::Kind::string&&id.kind!=Json::Kind::number)throw std::invalid_argument("resume request requires id");
    }
    auto kind=(started||resumed)?AgentEventKind::lifecycle:AgentEventKind::content;
    if(method=="turn/start"||method=="turn/steer"){
        const auto& id=parsed.at("id");
        if(id.kind!=Json::Kind::string&&id.kind!=Json::Kind::number)throw std::invalid_argument("input request requires id");
        if(method=="turn/steer"&&params.at("expectedTurnId").string().empty())throw std::invalid_argument("empty expected turn");
        const auto& input=params.at("input");
        if(input.kind!=Json::Kind::array)throw std::invalid_argument("input must be an array");
        for(const auto& item:input.values){
            const auto type=item.at("type").string();
            if(type.empty())throw std::invalid_argument("empty input type");
            if(type=="text")(void)item.at("text").string();
            // Keep image/audio/skill/mention and future fields exactly. No
            // attachment is fetched or opened by syntax adaptation.
        }
        kind=AgentEventKind::input;
    }
    AgentEvent event(bytes,std::move(parsed),kind,memory);event.app_server_=true;
    return event;
}
// Connection identity comes from the transport owner. Never manufacture a
// threadId in native bytes or downgrade a malformed user input to content.
inline AgentEvent AgentEvent::from_app_server_connection(std::string_view bytes,Json parsed,
    std::string_view connection,std::pmr::memory_resource& memory){
    using architecture::kernel::AgentEventKind;
    if(connection.empty())throw std::invalid_argument("connection binding required");
    const auto method=parsed.at("method").string();
    if(method.empty()||method=="turn/start"||method=="turn/steer"||method=="thread/started"||method=="thread/resume")
        throw std::invalid_argument("thread event requires thread binding");
    if(parsed.find("result")||parsed.find("error"))throw std::invalid_argument("method contains response fields");
    if(const auto* params=parsed.find("params");params&&params->kind!=Json::Kind::null){
        if(params->kind!=Json::Kind::object||params->find("threadId"))
            throw std::invalid_argument("connection event contains invalid or thread parameters");
    }
    AgentEvent event(bytes,std::move(parsed),AgentEventKind::content,memory);
    event.app_server_=true;event.bound_session_=connection;
    return event;
}
inline AgentEvent adapt_codex_app_server_connection(std::string_view bytes,std::string_view connection,
    std::pmr::memory_resource& memory){
    return AgentEvent::from_app_server_connection(bytes,parse_json(bytes,memory),connection,memory);
}
inline AgentEvent adapt_codex_app_server(std::string_view bytes,std::pmr::memory_resource& memory){
    return AgentEvent::from_app_server(bytes,parse_json(bytes,memory),memory);
}
// The transport already owns the native frame in this same budget. Transfer it
// only after the ordinary syntax and binding checks; retain exact wire bytes.
inline AgentEvent adapt_owned_codex_app_server(std::pmr::string bytes,
    std::pmr::memory_resource& memory,std::string_view connection={}){
    if(bytes.get_allocator().resource()!=&memory)
        throw std::invalid_argument("native frame allocator mismatch");
    auto parsed=parse_json(bytes,memory);
    auto event=connection.empty()?AgentEvent::from_app_server({},std::move(parsed),memory):
        AgentEvent::from_app_server_connection({},std::move(parsed),connection,memory);
    event.native_=std::move(bytes);
    return event;
}
} // namespace swegca::transport
