#pragma once
#include "transport/json.hpp"
#include "transport/socket_frames.hpp"
#include <chrono>
#include <cstdlib>
#include <poll.h>
#include <sys/un.h>

namespace swegca::transport {
inline const char* agent_query_endpoint() noexcept{
    const char* path=std::getenv("SWEGCA_QUERY_SOCKET");return path&&*path?path:nullptr;
}
constexpr std::string_view agent_replay_tool=R"({"name":"vrs_replay","description":"Ask the owning SWEGCA VRS to recall and replay one experience for the current input reference. Requires receipt and inputOriginal from the VRS input context. Optional scope is the exact observation scope. Does not add observations, end sessions, merge Main or authorize actions.","annotations":{"readOnlyHint":true,"destructiveHint":false},"inputSchema":{"type":"object","properties":{"receipt":{"type":"string"},"inputOriginal":{"type":"object","properties":{"block":{"type":"string"},"offset":{"type":"string"},"bytes":{"type":"string"},"digest":{"type":"string"}},"required":["block","offset","bytes","digest"],"additionalProperties":false},"related":{"type":"boolean","description":"Follow observations linked to the automatically selected parent original, without a scope name. Cannot combine with scope."},"scope":{"type":"string","minLength":1}},"required":["receipt","inputOriginal"],"additionalProperties":false}})";
inline std::pmr::string query_agent_replay(std::string_view id,const Json& arguments,std::pmr::memory_resource& memory){
    const auto* path=agent_query_endpoint();if(!path)throw std::runtime_error("owning VRS query endpoint unavailable");
    sockaddr_un endpoint{};endpoint.sun_family=AF_UNIX;
    const std::string_view name(path);
    if(name.empty()||name.front()!='/'||name.size()>=sizeof(endpoint.sun_path))
        throw std::invalid_argument("invalid owning VRS endpoint");
    std::copy_n(path,name.size()+1,endpoint.sun_path);
    struct Socket {int fd;~Socket(){if(fd>=0)::close(fd);}} socket{
        ::socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0)};
    if(socket.fd<0)throw std::system_error(errno,std::generic_category(),"query client socket");
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);
    const auto wait=[&](short events){
        for(;;){
            const auto left=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-std::chrono::steady_clock::now()).count();
            if(left<=0)throw std::runtime_error("owning VRS query timeout");
            pollfd ready{socket.fd,events,0};const int count=::poll(&ready,1,static_cast<int>(left));
            if(count<0&&errno==EINTR)continue;
            if(count<0)throw std::system_error(errno,std::generic_category(),"query client poll");
            if(!count)throw std::runtime_error("owning VRS query timeout");
            return;
        }
    };
    if(::connect(socket.fd,reinterpret_cast<sockaddr*>(&endpoint),sizeof(endpoint))){
        if(errno!=EINPROGRESS)throw std::system_error(errno,std::generic_category(),"query connect");
        wait(POLLOUT);int error=0;socklen_t size=sizeof(error);
        if(::getsockopt(socket.fd,SOL_SOCKET,SO_ERROR,&error,&size)||error)
            throw std::runtime_error("owning VRS connection failed");
    }
    ucred peer{};socklen_t size=sizeof(peer);
    if(::getsockopt(socket.fd,SOL_SOCKET,SO_PEERCRED,&peer,&size)||peer.uid!=::geteuid())
        throw std::runtime_error("owning VRS peer mismatch");
    std::pmr::string request("{\"jsonrpc\":\"2.0\",\"id\":",&memory);
    request+=id;request+=",\"method\":\"swegca/agent/replay\",\"params\":";
    append_json(request,arguments);request+="}\n";
    if(request.size()>65536)throw std::length_error("query request too large");
    for(std::size_t sent=0;sent<request.size();){
        const auto count=::send(socket.fd,request.data()+sent,request.size()-sent,MSG_NOSIGNAL);
        if(count>0)sent+=static_cast<std::size_t>(count);
        else if(count<0&&(errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR))wait(POLLOUT);
        else throw std::runtime_error("owning VRS query write failed");
    }
    SocketFrames frames(socket.fd,64U<<20,memory);
    for(;;){
        const auto state=frames.poll();
        if(state==SocketFrames::State::end)throw std::runtime_error("owning VRS query closed without response");
        if(state==SocketFrames::State::frame){
            const auto reply=parse_json(frames.frame(),memory);
            if(reply.at("jsonrpc").string()!="2.0"||encode_json(reply.at("id"),memory)!=id||
                bool(reply.find("result"))==bool(reply.find("error")))
                throw std::runtime_error("owning VRS reply mismatch");
            return std::pmr::string(frames.frame(),&memory);
        }
        if(!frames.buffered())wait(POLLIN);
    }
}
} // namespace swegca::transport
