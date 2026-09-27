#pragma once
#include "transport/app_server_requests.hpp"
#include <memory>
#include <variant>
#include <cerrno>
#include <system_error>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace swegca::transport {
// One serialized app-server connection. Plans contain transport facts only;
// SWEGCA judgment and recording remain in the existing Main-owned VRS endpoint.
class AppServerWire final {
    struct Identity {
        // Serialized owner only. A destroyed partial frame deliberately leaves
        // its lane occupied: continuing that stream would corrupt JSON framing.
        mutable std::array<std::uint64_t,2> active{0,0};
        mutable std::uint64_t next=0;
    };
public:
    class Delivery final {
    public:
        Delivery(const Delivery&)=delete;
        Delivery& operator=(const Delivery&)=delete;
        Delivery(Delivery&& other) noexcept
            :value_(std::move(other.value_)),sender_(other.sender_),sequence_(other.sequence_),observed_(other.observed_),
             owner_(std::move(other.owner_)),recorded_(other.recorded_),socket_(std::exchange(other.socket_,-1)),
             offset_(other.offset_),newline_(other.newline_),ticket_(other.ticket_){}
        ~Delivery(){
            if(socket_>=0){
                ::close(socket_);
                const auto lane=sender_==RpcSender::client?0:1;
                if(!offset_&&owner_&&owner_->active[lane]==ticket_)owner_->active[lane]=0;
            }
        }
        [[nodiscard]] std::size_t sent_bytes() const noexcept{return offset_+(newline_?1:0);}
        [[nodiscard]] const AgentEvent& event() const noexcept{
            if(const auto* response=std::get_if<AppServerRequests::Response>(&value_))return response->event();
            return std::get<AgentEvent>(value_);
        }
        [[nodiscard]] std::uint64_t sequence() const noexcept{return sequence_;}
        [[nodiscard]] std::uint64_t observed_at() const noexcept{return observed_;}
        [[nodiscard]] std::optional<std::uint64_t> request_sequence() const noexcept{
            if(const auto* response=std::get_if<AppServerRequests::Response>(&value_))return response->request_sequence();
            return std::nullopt;
        }
    private:
        friend class AppServerWire;
        using Value=std::variant<AgentEvent,AppServerRequests::Response>;
        Delivery(Value value,RpcSender sender,std::uint64_t sequence,std::uint64_t observed,std::shared_ptr<const Identity> owner)
            :value_(std::move(value)),sender_(sender),sequence_(sequence),observed_(observed),owner_(std::move(owner)){}
        Value value_;
        RpcSender sender_;
        std::uint64_t sequence_,observed_;
        std::shared_ptr<const Identity> owner_;
        bool recorded_=false;
        int socket_=-1;
        std::size_t offset_=0;
        bool newline_=false;
        std::uint64_t ticket_=0;
    };
    AppServerWire(std::pmr::memory_resource& memory,std::size_t sessions,std::size_t requests)
        :memory_(memory),capacity_(sessions),identity_(std::allocate_shared<Identity>(std::pmr::polymorphic_allocator<Identity>(&memory))),
         sessions_(&memory),connection_(&memory),requests_(memory,requests){
        if(!sessions)throw std::invalid_argument("wire session capacity must be positive");
    }
    AppServerWire(const AppServerWire&)=delete;
    AppServerWire& operator=(const AppServerWire&)=delete;
    // The caller gets nextSequence from a successful VRS attach/resume result.
    void attach(std::string_view session,std::uint64_t next){
        if(session.empty()||sessions_.contains(session))throw std::invalid_argument("invalid or duplicate wire session");
        if(sessions_.size()==capacity_)throw std::length_error("wire session capacity exhausted");
        sessions_.try_emplace(std::pmr::string(session,&memory_),next);
    }
    // A separate VRS session, already attached by the owner, for global RPCs.
    // Reserve its name so no native thread can impersonate this route.
    void attach_connection(std::string_view session,std::uint64_t next){
        if(!connection_.empty())throw std::invalid_argument("connection already attached");
        std::pmr::string owned(session,&memory_);
        attach(session,next);connection_.swap(owned);
    }
    [[nodiscard]] Delivery prepare(std::string_view native,RpcSender sender,std::uint64_t observed){
        return prepare(native,sender,observed,[](const AgentEvent&)->std::uint64_t{throw std::invalid_argument("wire session has not been attached to VRS");});
    }
    template<class Bind>
    [[nodiscard]] Delivery prepare(std::string_view native,RpcSender sender,std::uint64_t observed,Bind&& bind){
        if(sender!=RpcSender::client&&sender!=RpcSender::server)throw std::invalid_argument("invalid wire sender");
        auto fields=parse_json(native,memory_);
        Delivery::Value value=fields.find("method")?
            Delivery::Value(adapt_method(native,std::move(fields))):
            Delivery::Value(requests_.bind_parsed(native,std::move(fields),sender));
        const auto& event=std::holds_alternative<AgentEvent>(value)?std::get<AgentEvent>(value):std::get<AppServerRequests::Response>(value).event();
        if(event.kind()==architecture::kernel::AgentEventKind::input&&sender!=RpcSender::client)
            throw std::invalid_argument("input request must originate from client");
        if(event.native_name()=="thread/resume"&&sender!=RpcSender::client)
            throw std::invalid_argument("thread resume must originate from client");
        if(event.native_name()=="thread/started"&&sender!=RpcSender::server)
            throw std::invalid_argument("thread started must originate from server");
        if(!std::holds_alternative<AppServerRequests::Response>(value)&&event.fields().find("id"))
            requests_.require_available(sender,event);
        auto found=sessions_.find(event.session());
        if(found==sessions_.end()){
            if(sessions_.size()==capacity_)throw std::length_error("wire session capacity exhausted");
            // A native thread identity on a content/lifecycle envelope is
            // sufficient for storage attachment. Never discover from user input
            // or invent a binding for a response without its original request.
            using namespace architecture::kernel;
            if(std::holds_alternative<AppServerRequests::Response>(value)||
               route_agent_event(SessionPhase::active,event.kind())!=AgentEventRoute::record)
                throw std::invalid_argument("session binding required before input");
            attach(event.session(),bind(event));found=sessions_.find(event.session());
        }
        if(found->second==std::numeric_limits<std::uint64_t>::max())throw std::overflow_error("delivery sequence exhausted");
        if(const auto* response=std::get_if<AppServerRequests::Response>(&value);response&&!response->request_sequence())
            throw std::invalid_argument("response request has no committed sequence");
        return Delivery(std::move(value),sender,found->second,observed,identity_);
    }
    // Recheck if lifecycle recovery installed pending requests after prepare.
    void preflight(const Delivery& delivery) const {
        validate(delivery);
        const auto found=sessions_.find(delivery.event().session());
        if(found==sessions_.end()||found->second!=delivery.sequence_)throw std::invalid_argument("stale wire delivery");
        if(!std::holds_alternative<AppServerRequests::Response>(delivery.value_)&&delivery.event().fields().find("id"))
            requests_.require_available(delivery.sender_,delivery.event());
    }
    // Only after the matching VRS RPC confirms original ingestion. If request
    // tracking allocation fails, leave the plan retryable and do not forward.
    void recorded(Delivery& delivery){
        validate(delivery);
        const auto found=sessions_.find(delivery.event().session());
        if(found==sessions_.end()||found->second!=delivery.sequence_)throw std::invalid_argument("stale wire delivery");
        if(const auto* response=std::get_if<AppServerRequests::Response>(&delivery.value_))requests_.recorded(*response);
        else if(delivery.event().fields().find("id"))requests_.track(delivery.sender_,delivery.event(),delivery.sequence_);
        ++found->second;delivery.recorded_=true;
    }
    [[nodiscard]] std::string_view forward(const Delivery& delivery) const{
        if(delivery.owner_!=identity_||!delivery.recorded_)throw std::invalid_argument("VRS ingestion confirmation required");
        return delivery.event().native_bytes();
    }
    // Bind one stream socket to this confirmed frame. Owning a duplicated fd
    // prevents descriptor reuse/reconnection from silently resuming a prefix on
    // another stream. Does not change caller fd flags or process SIGPIPE state.
    void bind_socket(Delivery& delivery,int socket) const{
        const auto native=forward(delivery);
        if(delivery.socket_>=0||delivery.offset_||delivery.newline_)throw std::invalid_argument("frame socket already bound");
        if(native.find_first_of("\r\n")!=std::string_view::npos)
            throw std::invalid_argument("app-server stream requires a single JSON line without delimiter");
        const auto lane=delivery.sender_==RpcSender::client?0:1;
        if(identity_->active[lane])throw std::logic_error("prior frame still owns this wire direction");
        if(identity_->next==std::numeric_limits<std::uint64_t>::max())throw std::overflow_error("frame ticket exhausted");
        int type=0;socklen_t size=sizeof(type);
        if(::getsockopt(socket,SOL_SOCKET,SO_TYPE,&type,&size)<0)throw std::system_error(errno,std::generic_category(),"frame socket type");
        if(type!=SOCK_STREAM)throw std::invalid_argument("frame requires a stream socket");
        const int owned=::fcntl(socket,F_DUPFD_CLOEXEC,0);
        if(owned<0)throw std::system_error(errno,std::generic_category(),"duplicate frame socket");
        delivery.socket_=owned;delivery.ticket_=++identity_->next;identity_->active[lane]=delivery.ticket_;
    }
    // One nonblocking send attempt. False means more readiness/drain is needed;
    // successful prefixes remain consumed through EAGAIN/EINTR or peer failure.
    // Exactly one line delimiter follows the byte-exact original JSON.
    [[nodiscard]] bool send_ready(Delivery& delivery) const{
        const auto native=forward(delivery);
        if(delivery.newline_)return true;
        if(delivery.socket_<0)throw std::invalid_argument("frame socket not bound");
        const auto lane=delivery.sender_==RpcSender::client?0:1;
        if(identity_->active[lane]!=delivery.ticket_)throw std::logic_error("frame no longer owns wire direction");
        const bool body=delivery.offset_<native.size();
        const auto remaining=body?native.substr(delivery.offset_):std::string_view("\n");
        const auto count=::send(delivery.socket_,remaining.data(),remaining.size(),MSG_DONTWAIT|MSG_NOSIGNAL);
        if(count<0){
            const auto code=errno;
            if(code==EAGAIN||code==EWOULDBLOCK||code==EINTR)return false;
            throw std::system_error(code,std::generic_category(),"forward app-server frame");
        }
        if(count==0)throw std::system_error(EPIPE,std::generic_category(),"zero-byte frame send");
        if(body)delivery.offset_+=static_cast<std::size_t>(count);
        else {delivery.newline_=true;::close(delivery.socket_);delivery.socket_=-1;identity_->active[lane]=0;}
        return delivery.newline_;
    }
    [[nodiscard]] std::pmr::string parameters(const Delivery& delivery,std::uint64_t seed,std::uint64_t step) const{
        validate(delivery);
        auto body=std::pmr::string("{\"sequence\":\"",&memory_)+std::to_string(delivery.sequence_).c_str()+
            "\",\"observedAt\":\""+std::to_string(delivery.observed_).c_str()+"\",\"seed\":\""+
            std::to_string(seed).c_str()+"\",\"step\":\""+std::to_string(step).c_str()+"\",\"native\":"+
            quote_json(delivery.event().native_bytes(),memory_);
        body+=delivery.sender_==RpcSender::client?",\"sender\":\"client\"":",\"sender\":\"server\"";
        if(const auto sequence=delivery.request_sequence()){body+=",\"requestSequence\":\"";body+=std::to_string(*sequence);body+='"';}
        body+='}';return body;
    }
    [[nodiscard]] std::size_t pending_requests() const noexcept{return requests_.pending();}
    // Recovery caller must supply an authenticated committed request and its
    // observed direction; this never invents bindings from unknown responses.
    void restore_request(RpcSender sender,const AgentEvent& event,std::uint64_t sequence){
        const auto found=sessions_.find(event.session());
        if(found==sessions_.end()||sequence>=found->second)throw std::invalid_argument("request is not a prior committed delivery");
        requests_.track(sender,event,sequence);
    }
private:
    AgentEvent adapt_method(std::string_view native,Json fields){
        const auto method=fields.at("method").string();
        const auto* params=fields.find("params");
        const bool thread=method=="turn/start"||method=="turn/steer"||method=="thread/started"||method=="thread/resume"||
            (params&&params->find("threadId"));
        if(thread){
            auto event=AgentEvent::from_app_server(native,std::move(fields),memory_);
            if(!connection_.empty()&&event.session()==connection_)
                throw std::invalid_argument("thread collides with connection binding");
            return event;
        }
        return AgentEvent::from_app_server_connection(native,std::move(fields),connection_,memory_);
    }
    void validate(const Delivery& delivery) const{
        if(delivery.owner_!=identity_||delivery.recorded_)throw std::invalid_argument("foreign or recorded wire delivery");
    }
    std::pmr::memory_resource& memory_;
    std::size_t capacity_;
    std::shared_ptr<const Identity> identity_;
    std::pmr::map<std::pmr::string,std::uint64_t,std::less<>> sessions_;
    std::pmr::string connection_;
    AppServerRequests requests_;
};
} // namespace swegca::transport
