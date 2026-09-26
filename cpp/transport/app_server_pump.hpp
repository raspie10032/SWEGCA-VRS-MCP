#pragma once
#include "transport/app_server_wire.hpp"
#include "transport/socket_frames.hpp"

namespace swegca::transport {
// Serialized, readiness-driven duplex pump. The callback must confirm the
// matching durable VRS request; it must not run models or forward native frames.
class AppServerPump final {
public:
    enum class State { idle, waiting_for_record, forwarding, forwarded, end };
    AppServerPump(int client,int server,std::size_t frame_limit,std::pmr::memory_resource& memory,
        std::size_t sessions,std::size_t requests)
        :client_(client,frame_limit,memory),server_(server,frame_limit,memory),wire_(memory,sessions,requests){}
    AppServerPump(const AppServerPump&)=delete;
    AppServerPump& operator=(const AppServerPump&)=delete;
    void attach(std::string_view session,std::uint64_t next){wire_.attach(session,next);}
    void restore_request(RpcSender sender,const AgentEvent& event,std::uint64_t sequence){wire_.restore_request(sender,event,sequence);}
    [[nodiscard]] std::size_t pending_requests() const noexcept{return wire_.pending_requests();}
    [[nodiscard]] bool buffered(RpcSender sender) const{
        if(sender!=RpcSender::client&&sender!=RpcSender::server)throw std::invalid_argument("invalid pump sender");
        return sender==RpcSender::client?client_.buffered():server_.buffered();
    }
    [[nodiscard]] std::pmr::string parameters(const AppServerWire::Delivery& delivery,std::uint64_t seed,std::uint64_t step) const{
        return wire_.parameters(delivery,seed,step);
    }
    // One direction per call; the event-loop owner schedules both fairly.
    // Ingestion is globally serialized across directions to avoid assigning two
    // different events the same session sequence during an acknowledgement retry.
    template<class Ingest>
    [[nodiscard]] State step(RpcSender sender,std::uint64_t observed,Ingest&& ingest){
        if(sender!=RpcSender::client&&sender!=RpcSender::server)throw std::invalid_argument("invalid pump sender");
        const auto lane=sender==RpcSender::client?0:1;
        auto& stage=stages_[lane];auto& reader=lane==0?client_:server_;
        if(!stage.delivery){
            if(ingress_&&*ingress_!=sender)return State::waiting_for_record;
            const auto state=reader.poll();
            if(state==SocketFrames::State::pending)return State::idle;
            if(state==SocketFrames::State::end)return State::end;
            stage.delivery.emplace(wire_.prepare(reader.frame(),sender,observed));
            ingress_=sender;reader.consumed();
        }
        if(!stage.ingested){
            if(!ingest(*stage.delivery))return State::waiting_for_record;
            stage.ingested=true;
        }
        if(!stage.recorded){
            wire_.recorded(*stage.delivery);stage.recorded=true;ingress_.reset();
        }
        if(!stage.bound){
            wire_.bind_socket(*stage.delivery,lane==0?server_.fd_:client_.fd_);stage.bound=true;
        }
        if(!wire_.send_ready(*stage.delivery))return State::forwarding;
        stage.delivery.reset();stage.ingested=stage.recorded=stage.bound=false;
        return State::forwarded;
    }
private:
    struct Stage {
        std::optional<AppServerWire::Delivery> delivery;
        bool ingested=false,recorded=false,bound=false;
    };
    SocketFrames client_,server_;
    AppServerWire wire_;
    std::array<Stage,2> stages_;
    std::optional<RpcSender> ingress_;
};
} // namespace swegca::transport
