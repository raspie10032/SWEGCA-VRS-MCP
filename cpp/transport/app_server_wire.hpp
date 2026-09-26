#pragma once
#include "transport/app_server_requests.hpp"
#include <memory>
#include <variant>

namespace swegca::transport {
// One serialized app-server connection. Plans contain transport facts only;
// SWEGCA judgment and recording remain in the existing Main-owned VRS endpoint.
class AppServerWire final {
    struct Identity {};
public:
    class Delivery final {
    public:
        Delivery(const Delivery&)=delete;
        Delivery& operator=(const Delivery&)=delete;
        Delivery(Delivery&&) noexcept=default;
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
    };
    AppServerWire(std::pmr::memory_resource& memory,std::size_t sessions,std::size_t requests)
        :memory_(memory),capacity_(sessions),request_capacity_(requests),identity_(std::allocate_shared<Identity>(std::pmr::polymorphic_allocator<Identity>(&memory))),
         sessions_(&memory),requests_(memory,requests){
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
    [[nodiscard]] Delivery prepare(std::string_view native,RpcSender sender,std::uint64_t observed){
        if(sender!=RpcSender::client&&sender!=RpcSender::server)throw std::invalid_argument("invalid wire sender");
        auto fields=parse_json(native,memory_);
        Delivery::Value value=fields.find("method")?
            Delivery::Value(AgentEvent::from_app_server(native,std::move(fields),memory_)):
            Delivery::Value(requests_.bind_parsed(native,std::move(fields),sender));
        const auto& event=std::holds_alternative<AgentEvent>(value)?std::get<AgentEvent>(value):std::get<AppServerRequests::Response>(value).event();
        if(event.kind()==architecture::kernel::AgentEventKind::input&&sender!=RpcSender::client)
            throw std::invalid_argument("input request must originate from client");
        if(!std::holds_alternative<AppServerRequests::Response>(value)&&event.fields().find("id")&&requests_.pending()==request_capacity_)
            throw std::length_error("pending request capacity exhausted");
        const auto found=sessions_.find(event.session());
        if(found==sessions_.end())throw std::invalid_argument("wire session has not been attached to VRS");
        if(found->second==std::numeric_limits<std::uint64_t>::max())throw std::overflow_error("delivery sequence exhausted");
        if(const auto* response=std::get_if<AppServerRequests::Response>(&value);response&&!response->request_sequence())
            throw std::invalid_argument("response request has no committed sequence");
        return Delivery(std::move(value),sender,found->second,observed,identity_);
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
    [[nodiscard]] std::pmr::string parameters(const Delivery& delivery,std::uint64_t seed,std::uint64_t step) const{
        validate(delivery);
        auto body=std::pmr::string("{\"sequence\":\"",&memory_)+std::to_string(delivery.sequence_).c_str()+
            "\",\"observedAt\":\""+std::to_string(delivery.observed_).c_str()+"\",\"seed\":\""+
            std::to_string(seed).c_str()+"\",\"step\":\""+std::to_string(step).c_str()+"\",\"native\":"+
            quote_json(delivery.event().native_bytes(),memory_);
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
    void validate(const Delivery& delivery) const{
        if(delivery.owner_!=identity_||delivery.recorded_)throw std::invalid_argument("foreign or recorded wire delivery");
    }
    std::pmr::memory_resource& memory_;
    std::size_t capacity_,request_capacity_;
    std::shared_ptr<const Identity> identity_;
    std::pmr::map<std::pmr::string,std::uint64_t,std::less<>> sessions_;
    AppServerRequests requests_;
};
} // namespace swegca::transport
