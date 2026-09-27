#include "transport/app_server_pump.hpp"
#include "transport/agent_event_commit.hpp"
#include "vrs/memory_budget.hpp"
#include <chrono>
#include <climits>
#include <fstream>
#include <iostream>
#include <map>
#include <poll.h>

using namespace swegca::transport;
namespace {
std::uint64_t number(std::string_view text){
    std::uint64_t out=0;const auto result=std::from_chars(text.data(),text.data()+text.size(),out);
    if(text.empty()||result.ec!=std::errc{}||result.ptr!=text.data()+text.size())throw std::invalid_argument("invalid unsigned decimal");
    return out;
}
int descriptor(const char* text){const auto value=number(text);if(value<3||value>INT_MAX)throw std::invalid_argument("dedicated descriptor above stderr required");return static_cast<int>(value);}
void wait_ready(std::span<pollfd> descriptors){
    for(;;){
        const auto count=::poll(descriptors.data(),descriptors.size(),-1);
        if(count<0&&errno==EINTR)continue;
        if(count<0)throw std::system_error(errno,std::generic_category(),"proxy poll");
        for(const auto& fd:descriptors)if(fd.revents&POLLNVAL)throw std::runtime_error("proxy descriptor closed");
        return;
    }
}
// Dedicated inherited VRS stream. Synchronous storage RPCs intentionally apply
// backpressure; there are no background model calls or implicit reconnections.
class VrsStream final {
public:
    VrsStream(int fd,std::size_t limit,std::pmr::memory_resource& memory):fd_(fd),reader_(fd,limit,memory),memory_(memory),limit_(limit){}
    void send(std::string_view frame){
        if(frame.size()>limit_)throw std::length_error("VRS RPC exceeds configured frame limit");
        if(frame.find_first_of("\r\n")!=std::string_view::npos)throw std::invalid_argument("single JSON line required");
        write(frame);write("\n");
    }
    std::pmr::string exchange(std::string_view frame){
        send(frame);
        for(;;){
            const auto state=reader_.poll();
            if(state==SocketFrames::State::end)throw std::runtime_error("VRS disconnected before acknowledgement");
            if(state==SocketFrames::State::frame){std::pmr::string reply(reader_.frame(),&memory_);reader_.consumed();return reply;}
            if(!reader_.buffered()){pollfd fd{fd_,POLLIN,0};wait_ready({&fd,1});}
        }
    }
private:
    void write(std::string_view bytes){
        while(!bytes.empty()){
            const auto sent=::send(fd_,bytes.data(),bytes.size(),MSG_DONTWAIT|MSG_NOSIGNAL);
            if(sent>0){bytes.remove_prefix(static_cast<std::size_t>(sent));continue;}
            if(sent<0&&errno==EINTR)continue;
            if(sent<0&&(errno==EAGAIN||errno==EWOULDBLOCK)){pollfd fd{fd_,POLLOUT,0};wait_ready({&fd,1});continue;}
            throw std::runtime_error("VRS request send failed");
        }
    }
    int fd_;SocketFrames reader_;std::pmr::memory_resource& memory_;std::size_t limit_;
};
Json call(VrsStream& stream,std::string_view id,std::string_view method,std::string_view params,std::pmr::memory_resource& memory){
    auto reply=parse_json(stream.exchange("{\"jsonrpc\":\"2.0\",\"id\":"+quote_json(id,memory)+",\"method\":"+
        quote_json(method,memory)+",\"params\":"+std::pmr::string(params,&memory)+"}"),memory);
    if(reply.at("jsonrpc").string()!="2.0"||reply.at("id").string()!=id||reply.find("error")||reply.find("method")||reply.at("result").kind!=Json::Kind::object)
        throw std::runtime_error("VRS setup not acknowledged");
    return reply;
}
}
int main(int argc,char** argv){
    try{
        if(argc!=5)throw std::invalid_argument("usage: swegca-app-server-proxy CLIENT_FD SERVER_FD VRS_FD CONFIG");
        const int client=descriptor(argv[1]),server=descriptor(argv[2]),vrs=descriptor(argv[3]);
        if(client==server||client==vrs||server==vrs)throw std::invalid_argument("distinct exclusive streams required");
        swegca::vrs::MemoryBudget config_memory(1<<20);std::ifstream file(argv[4]);
        if(!file)throw std::runtime_error("cannot open proxy configuration");
        std::pmr::string text(&config_memory);char c;
        while(file.get(c)){if(text.size()==65536)throw std::length_error("proxy configuration too large");text+=c;}
        const auto config=parse_json(text,config_memory);
        const auto ram=number(config.at("memoryBytes").string()),frame=number(config.at("frameBytes").string());
        const auto capacity=number(config.at("pendingRequests").string());
        const auto seed=number(config.at("seed").string()),step=number(config.at("step").string());
        if(!ram||!frame||frame>ram||!capacity)throw std::invalid_argument("invalid proxy limits");
        // Native JSON is quoted inside RPC JSON. Reserve worst-case escaping
        // plus receipt/setup metadata, without enlarging native acceptance.
        if(frame>(UINT64_MAX-65536)/6)throw std::overflow_error("native frame expansion overflow");
        const auto minimum_rpc_frame=frame*6+65536;
        const auto* configured_rpc_frame=config.find("vrsFrameBytes");
        const auto rpc_frame=configured_rpc_frame?number(configured_rpc_frame->string()):minimum_rpc_frame;
        if(rpc_frame<minimum_rpc_frame||rpc_frame>ram)throw std::invalid_argument("invalid VRS RPC frame budget");
        const auto& sessions=config.at("sessions");
        if(sessions.kind!=Json::Kind::array)throw std::invalid_argument("session bindings must be an array");
        const auto* connection=config.find("connectionSession");
        const auto initial_sessions=sessions.values.size()+(connection?1:0);
        const auto* configured_sessions=config.find("sessionCapacity");
        const auto session_capacity=configured_sessions?number(configured_sessions->string()):initial_sessions;
        if(!session_capacity||session_capacity<initial_sessions)throw std::invalid_argument("invalid session capacity");
        swegca::vrs::MemoryBudget memory(ram);
        AppServerPump pump(client,server,frame,memory,session_capacity,capacity);
        VrsStream stream(vrs,rpc_frame,memory);
        const auto initialized=call(stream,"proxy/initialize","initialize",R"({"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"swegca-app-server-proxy","version":"0.1"}})",memory);
        const auto& result=initialized.at("result");
        if(result.at("protocolVersion").string()!="2025-06-18"||result.at("capabilities").at("experimental").at("swegcaHostInput").at("version").string()!="14")
            throw std::runtime_error("unsupported VRS host protocol");
        if(number(result.at("capabilities").at("experimental").at("swegcaHostInput").at("frameBytes").string())<rpc_frame)
            throw std::runtime_error("VRS host frame budget smaller than proxy RPC budget");
        stream.send(R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
        std::pmr::map<std::pmr::string,std::pmr::string,std::less<>> bindings(&memory);
        std::uint64_t serial=0;
        // Rebuild each configured session independently so settled IDs reused
        // by another session cannot collide. Only unfinished requests enter
        // the live connection-wide table. Reads use verified VRS originals.
        const auto recover=[&](std::string_view identity,std::string_view name,
                               bool connection_scope,std::uint64_t next){
            struct Pending { AgentEvent event; RpcSender sender; std::uint64_t sequence; };
            std::pmr::map<std::pmr::string,Pending,std::less<>> unfinished(&memory);
            AppServerRequests requests(memory,capacity);
            for(std::uint64_t sequence=0;sequence<next;++sequence){
                if(serial==UINT64_MAX)throw std::overflow_error("proxy request IDs exhausted");
                const auto params="{\"identity\":"+quote_json(identity,memory)+",\"sequence\":\""+
                    std::to_string(sequence).c_str()+"\"}";
                const auto reply=call(stream,"proxy/recover/"+std::to_string(++serial),"swegca/agent/original",params,memory);
                const auto& original=reply.at("result");
                const auto direction=original.at("sender").string();
                if(direction!="client"&&direction!="server")throw std::runtime_error("recorded sender required for recovery");
                const auto sender=direction=="client"?RpcSender::client:RpcSender::server;
                const auto raw=original.at("native").string();
                const auto fields=parse_json(raw,memory);
                if(fields.find("method")){
                    auto event=connection_scope?adapt_codex_app_server_connection(raw,name,memory):adapt_codex_app_server(raw,memory);
                    if(event.session()!=name)throw std::runtime_error("recovered request session mismatch");
                    if(!fields.find("id"))continue;
                    requests.track(sender,event,sequence);
                    const auto digest=original.at("original").at("digest").string();
                    if(!unfinished.try_emplace(std::pmr::string(digest,&memory),Pending{std::move(event),sender,sequence}).second)
                        throw std::runtime_error("duplicate recovered original address");
                }else{
                    if(bool(fields.find("result"))==bool(fields.find("error")))
                        throw std::runtime_error("invalid recorded reply");
                    const auto found=unfinished.find(original.at("context").string());
                    // A verified repeated reply may refer to an already settled
                    // original. Never erase another request just because IDs match.
                    if(found==unfinished.end())continue;
                    const auto response=requests.bind(raw,sender);
                    if(response.request_sequence()!=found->second.sequence)
                        throw std::runtime_error("recovered response lineage mismatch");
                    requests.recorded(response);unfinished.erase(found);
                }
            }
            for(const auto& [digest,pending]:unfinished){
                (void)digest;pump.restore_request(pending.sender,pending.event,pending.sequence);
            }
        };
        const auto attach=[&](const Json& session,bool connection_scope){
            const auto name=session.at("session").string();
            const auto mode=session.at("mode").string();
            if(mode!="attach"&&mode!="resume"&&mode!="ensure")throw std::invalid_argument("invalid binding mode");
            if(bindings.contains(name))throw std::invalid_argument("duplicate session binding");
            const auto params="{\"provider\":\"codex\",\"protocol\":"+quote_json(connection_scope?"app-server-connection":"app-server",memory)+",\"instance\":"+quote_json(config.at("instance").string(),memory)+",\"session\":"+quote_json(name,memory)+"}";
            const auto attached=call(stream,"proxy/attach/"+std::to_string(++serial),mode=="ensure"?"swegca/agent/attach/ensure":mode=="attach"?"swegca/agent/attach":"swegca/agent/attach/resume",params,memory);
            const auto& body=attached.at("result");
            const auto next=number(body.at("nextSequence").string());
            if(connection_scope)pump.attach_connection(name,next);else pump.attach(name,next);
            bindings.try_emplace(std::pmr::string(name,&memory),body.at("identity").string());
            if(next)recover(body.at("identity").string(),name,connection_scope,next);
        };
        if(connection)attach(*connection,true);
        for(const auto& session:sessions.values)attach(session,false);
        struct Recovery { std::pmr::string identity,name; std::uint64_t next; };
        std::optional<Recovery> pending_recovery;
        const auto bind_lifecycle=[&](const AgentEvent& event){
            if(serial==UINT64_MAX)throw std::overflow_error("proxy request IDs exhausted");
            const auto name=event.session();
            const auto params="{\"provider\":\"codex\",\"protocol\":\"app-server\",\"instance\":"+quote_json(config.at("instance").string(),memory)+",\"session\":"+quote_json(name,memory)+"}";
            const auto attached=call(stream,"proxy/ensure/"+std::to_string(++serial),"swegca/agent/attach/ensure",params,memory);
            const auto& body=attached.at("result");const auto next=number(body.at("nextSequence").string());
            const auto identity=body.at("identity").string();
            const auto [where,inserted]=bindings.try_emplace(std::pmr::string(name,&memory),identity);
            if(!inserted&&where->second!=identity)throw std::runtime_error("changed lifecycle binding");
            if(next)pending_recovery.emplace(Recovery{std::pmr::string(identity,&memory),std::pmr::string(name,&memory),next});
            return next;
        };
        std::cout<<"ready\n"<<std::flush;
        std::array<bool,2> ended{};
        while(!ended[0]||!ended[1]){
            std::array<pollfd,2> readiness{{{client,0,0},{server,0,0}}};bool buffered=false;
            for(std::size_t lane=0;lane<2;++lane){
                if(ended[lane])continue;
                const auto sender=lane==0?RpcSender::client:RpcSender::server;
                const auto observed=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                const auto state=pump.step(sender,static_cast<std::uint64_t>(observed),[&](const AppServerWire::Delivery& plan){
                    // Wire has now attached the lifecycle-discovered session.
                    // Restore before acknowledging its notification, never from input.
                    if(pending_recovery){
                        recover(pending_recovery->identity,pending_recovery->name,false,pending_recovery->next);
                        pending_recovery.reset();
                        pump.preflight(plan);
                    }
                    if(serial==UINT64_MAX)throw std::overflow_error("proxy request IDs exhausted");
                    const auto found=bindings.find(plan.event().session());
                    if(found==bindings.end())throw std::runtime_error("unbound native session");
                    AgentEventCommit commit(found->second,pump.parameters(plan,seed,step),"proxy/event/"+std::to_string(++serial),memory);
                    while(commit.stage()!=AgentEventCommit::Stage::complete)commit.accept(stream.exchange(commit.request()));
                    return true;
                },bind_lifecycle);
                if(state==AppServerPump::State::end){
                    ended[lane]=true;
                    if(::shutdown(lane==0?server:client,SHUT_WR)<0)throw std::system_error(errno,std::generic_category(),"proxy half-close");
                }else if(state==AppServerPump::State::forwarding)readiness[1-lane].events|=POLLOUT;
                else{readiness[lane].events|=POLLIN;buffered|=pump.buffered(sender);}
            }
            if(!ended[0]||!ended[1]){
                if(buffered)continue;
                for(auto& fd:readiness)if(!fd.events)fd.fd=-1;
                wait_ready(readiness);
            }
        }
        // EOF is transport termination only. No end/work RPC is issued.
        return 0;
    }catch(const std::exception& e){std::cerr<<"proxy stopped: "<<e.what()<<'\n';return 1;}
}
