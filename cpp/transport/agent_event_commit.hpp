#pragma once
#include "transport/json.hpp"
#include <charconv>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace swegca::transport {
// Transport transaction on an initialized, exclusively owned VRS connection.
// The caller provides a connection-unique ID and an authenticated session identity.
// This checks endpoint acknowledgements; it makes no evidence/authority decision.
class AgentEventCommit final {
public:
    enum class Stage { event, complete };
    // Native bytes are borrowed only during construction; request/retry owns
    // its complete encoding and never retains a view into the caller's event.
    AgentEventCommit(std::string_view identity,Json parsed,std::string_view native,
        std::string_view id,std::pmr::memory_resource& memory)
        :memory_(memory),event_id_(id,&memory),event_(&memory),reply_(&memory){
        hex(identity);
        if(id.empty())throw std::invalid_argument("commit request ID required");
        if(parsed.kind!=Json::Kind::object||parsed.keys.size()!=parsed.values.size())
            throw std::invalid_argument("event parameters must be an object");
        for(std::size_t i=0;i<parsed.keys.size();++i)
            for(std::size_t j=0;j<i;++j)if(parsed.keys[i]==parsed.keys[j])
                throw std::invalid_argument("duplicate event parameter");
        if(parsed.find("identity"))throw std::invalid_argument("event target belongs to transport owner");
        if(parsed.find("native"))throw std::invalid_argument("native bytes belong to delivery owner");
        Json target(&memory);target.kind=Json::Kind::string;target.scalar=identity;
        parsed.keys.emplace_back("identity");parsed.values.push_back(std::move(target));
        event_id_+="/event";
        event_="{\"jsonrpc\":\"2.0\",\"id\":"+quote_json(event_id_,memory_)+
            ",\"method\":\"swegca/agent/event\",\"params\":";
        append_json(event_,parsed);event_.pop_back();
        event_+=",\"native\":";append_json_string(event_,native,2);event_+="}}";
    }
    AgentEventCommit(const AgentEventCommit&)=delete;
    AgentEventCommit& operator=(const AgentEventCommit&)=delete;
    [[nodiscard]] Stage stage() const noexcept{return stage_;}
    [[nodiscard]] std::string_view request() const{
        if(stage_==Stage::complete)throw std::logic_error("event already acknowledged");
        return event_;
    }
    // Invalid, unrelated and error replies never advance the transaction.
    // Retrying a lost event acknowledgement uses the same sequence and payload.
    void accept(std::string_view raw){
        if(stage_==Stage::complete)throw std::logic_error("event already acknowledged");
        const auto value=parse_json(raw,memory_);
        if(value.at("jsonrpc").string()!="2.0"||value.at("id").string()!=event_id_||value.find("method"))
            throw std::invalid_argument("unmatched VRS acknowledgement");
        if(value.find("error"))throw std::runtime_error("VRS request not confirmed");
        const auto& result=value.at("result");
        if(result.kind!=Json::Kind::object)throw std::invalid_argument("invalid VRS result");
        const auto& original=result.at("original");
        hex(original.at("block").string());hex(original.at("digest").string());
        const auto offset=number(original.at("offset").string());
        const auto bytes=number(original.at("bytes").string());
        if(!bytes||offset>std::numeric_limits<std::uint64_t>::max()-bytes)
            throw std::invalid_argument("invalid recorded original extent");
        // Preserve the full endpoint result (including Recall/receipt) before
        // exposing completion. Allocation failure leaves this request pending.
        reply_.assign(raw);stage_=Stage::complete;
    }
    [[nodiscard]] std::string_view reply() const{
        if(stage_!=Stage::complete)throw std::logic_error("event not acknowledged");
        return reply_;
    }
private:
    static void hex(std::string_view value){
        if(value.size()!=64)throw std::invalid_argument("invalid identity/digest length");
        for(const char c:value)if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))
            throw std::invalid_argument("invalid identity/digest hex");
    }
    static std::uint64_t number(std::string_view value){
        std::uint64_t result=0;const auto parsed=std::from_chars(value.data(),value.data()+value.size(),result);
        if(value.empty()||parsed.ec!=std::errc{}||parsed.ptr!=value.data()+value.size())
            throw std::invalid_argument("invalid original extent");
        return result;
    }
    std::pmr::memory_resource& memory_;
    std::pmr::string event_id_,event_,reply_;
    Stage stage_=Stage::event;
};
} // namespace swegca::transport
