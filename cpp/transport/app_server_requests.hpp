#pragma once
#include "transport/agent_event.hpp"
#include <charconv>
#include <atomic>
#include <limits>
#include <map>

namespace swegca::transport {
enum class RpcSender { client, server };

// One serialized wire connection's routing provenance. This table neither
// judges evidence nor authorizes VRS lifecycle. Its owner explicitly bounds it.
class AppServerRequests final {
public:
    class Response final {
    public:
        Response(const Response&)=delete;
        Response& operator=(const Response&)=delete;
        Response(Response&&) noexcept=default;
        [[nodiscard]] const AgentEvent& event() const noexcept{return event_;}
        [[nodiscard]] std::optional<std::uint64_t> request_sequence() const noexcept{return sequence_;}
    private:
        friend class AppServerRequests;
        Response(AgentEvent event,std::pmr::string key,std::uint64_t owner,std::uint64_t generation,std::optional<std::uint64_t> sequence)
            :event_(std::move(event)),key_(std::move(key)),owner_(owner),generation_(generation),sequence_(sequence){}
        AgentEvent event_;
        std::pmr::string key_;
        std::uint64_t owner_;
        std::uint64_t generation_;
        std::optional<std::uint64_t> sequence_;
    };
    AppServerRequests(std::pmr::memory_resource& memory,std::size_t capacity)
        :memory_(memory),capacity_(capacity),owner_(next_owner()),pending_(&memory){
        if(!capacity)throw std::invalid_argument("pending request capacity must be positive");
    }
    AppServerRequests(const AppServerRequests&)=delete;
    AppServerRequests& operator=(const AppServerRequests&)=delete;
    [[nodiscard]] std::size_t pending() const noexcept{return pending_.size();}

    // Reserve before forwarding a request, so its response cannot race ahead.
    // Exact re-registration is harmless; another live request with the same ID
    // is a conflict. String IDs and signed integer IDs occupy separate keys.
    void track(RpcSender sender,const AgentEvent& request,std::optional<std::uint64_t> sequence=std::nullopt){
        if(!request.is_app_server()||request.native_name().empty())throw std::invalid_argument("app-server request required");
        const auto& fields=request.fields();
        if(fields.find("result")||fields.find("error"))throw std::invalid_argument("request contains response fields");
        auto key=id_key(sender,fields.at("id"));
        architecture::Sha256 hash;hash.update(request.native_bytes());const auto digest=hash.finish();
        const auto found=pending_.find(key);
        if(found!=pending_.end()){
            if(found->second.digest!=digest||found->second.session!=request.session()||found->second.sequence!=sequence)
                throw std::invalid_argument("conflicting live request ID");
            return;
        }
        if(pending_.size()==capacity_)throw std::length_error("pending request capacity exhausted");
        if(generation_==std::numeric_limits<std::uint64_t>::max())throw std::overflow_error("request generation exhausted");
        pending_.try_emplace(std::move(key),request.session(),digest,generation_+1,sequence,memory_);
        ++generation_;
    }

    // Binding is read-only. Keep the request until the owner confirms durable
    // recording of this exact returned response. No selected-session fallback.
    [[nodiscard]] Response bind(std::string_view bytes,RpcSender sender) const{
        return bind_parsed(bytes,parse_json(bytes,memory_),sender);
    }
private:
    friend class AppServerWire;
    [[nodiscard]] Response bind_parsed(std::string_view bytes,Json parsed,RpcSender sender) const{
        if(parsed.kind!=Json::Kind::object||parsed.find("method")||
           bool(parsed.find("result"))==bool(parsed.find("error")))
            throw std::invalid_argument("invalid app-server response envelope");
        const auto origin=opposite(sender);
        auto key=id_key(origin,parsed.at("id"));
        const auto found=pending_.find(key);
        if(found==pending_.end())throw std::invalid_argument("response has no request binding");
        if(const auto* error=parsed.find("error")){
            if(error->kind!=Json::Kind::object||error->at("code").kind!=Json::Kind::number)
                throw std::invalid_argument("invalid app-server error");
            (void)error->at("message").string();
        }
        AgentEvent event(bytes,std::move(parsed),architecture::kernel::AgentEventKind::content,memory_);
        event.app_server_=true;event.bound_session_=found->second.session;
        return Response(std::move(event),std::move(key),owner_,found->second.generation,found->second.sequence);
    }
public:
    // Call only after VRS has recorded the response. A stale ticket cannot erase
    // a subsequent request that reused the ID, nor another connection's request.
    void recorded(const Response& response){
        if(response.owner_!=owner_)throw std::invalid_argument("foreign response binding");
        const auto found=pending_.find(response.key_);
        if(found==pending_.end()||found->second.generation!=response.generation_)
            throw std::invalid_argument("expired response binding");
        pending_.erase(found);
    }
private:
    static std::uint64_t next_owner(){
        static std::atomic<std::uint64_t> sequence{0};
        auto value=sequence.load(std::memory_order_relaxed);
        do {
            if(value==std::numeric_limits<std::uint64_t>::max())throw std::overflow_error("request owner sequence exhausted");
        } while(!sequence.compare_exchange_weak(value,value+1,std::memory_order_relaxed));
        return value+1;
    }
    struct Pending {
        Pending(std::string_view session,architecture::DigestBytes digest,std::uint64_t generation,std::optional<std::uint64_t> sequence,std::pmr::memory_resource& memory)
            :session(session,&memory),digest(digest),generation(generation),sequence(sequence){}
        std::pmr::string session;
        architecture::DigestBytes digest;
        std::uint64_t generation;
        std::optional<std::uint64_t> sequence;
    };
    static RpcSender opposite(RpcSender sender){
        switch(sender){case RpcSender::client:return RpcSender::server;case RpcSender::server:return RpcSender::client;}
        throw std::invalid_argument("invalid RPC sender");
    }
    std::pmr::string id_key(RpcSender sender,const Json& id) const{
        (void)opposite(sender);
        std::pmr::string key(sender==RpcSender::client?"c":"s",&memory_);
        if(id.kind==Json::Kind::string){key+='s';key+=id.string();}
        else if(id.kind==Json::Kind::number){
            std::int64_t number=0;
            const auto result=std::from_chars(id.scalar.data(),id.scalar.data()+id.scalar.size(),number);
            if(result.ec!=std::errc{}||result.ptr!=id.scalar.data()+id.scalar.size())throw std::invalid_argument("request ID must be int64 or string");
            key+='i';key+=std::to_string(number);
        }else throw std::invalid_argument("request ID must be int64 or string");
        return key;
    }
    std::pmr::memory_resource& memory_;
    std::size_t capacity_;
    std::uint64_t owner_;
    std::uint64_t generation_=0;
    std::pmr::map<std::pmr::string,Pending,std::less<>> pending_;
};
} // namespace swegca::transport
