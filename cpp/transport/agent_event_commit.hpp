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
    enum class Stage { select, event, complete };
    AgentEventCommit(std::string_view identity,std::string_view parameters,
        std::string_view id,std::pmr::memory_resource& memory)
        :memory_(memory),select_id_(id,&memory),event_id_(id,&memory),
         select_(&memory),event_(&memory),reply_(&memory){
        hex(identity);
        if(id.empty())throw std::invalid_argument("commit request ID required");
        const auto parsed=parse_json(parameters,memory);
        if(parsed.kind!=Json::Kind::object)
            throw std::invalid_argument("event parameters must be an object");
        select_id_+="/select";event_id_+="/event";
        select_=envelope(select_id_,"swegca/select","{\"identity\":"+quote_json(identity,memory)+"}");
        event_=envelope(event_id_,"swegca/agent/event",encode_json(parsed,memory));
    }
    AgentEventCommit(const AgentEventCommit&)=delete;
    AgentEventCommit& operator=(const AgentEventCommit&)=delete;
    [[nodiscard]] Stage stage() const noexcept{return stage_;}
    [[nodiscard]] std::string_view request() const{
        if(stage_==Stage::complete)throw std::logic_error("event already acknowledged");
        return stage_==Stage::select?select_:event_;
    }
    // Invalid, unrelated and error replies never advance the transaction.
    // Retrying a lost event acknowledgement uses the same sequence and payload.
    void accept(std::string_view raw){
        if(stage_==Stage::complete)throw std::logic_error("event already acknowledged");
        const auto value=parse_json(raw,memory_);
        const auto& id=stage_==Stage::select?select_id_:event_id_;
        if(value.at("jsonrpc").string()!="2.0"||value.at("id").string()!=id||value.find("method"))
            throw std::invalid_argument("unmatched VRS acknowledgement");
        if(value.find("error"))throw std::runtime_error("VRS request not confirmed");
        const auto& result=value.at("result");
        if(result.kind!=Json::Kind::object)throw std::invalid_argument("invalid VRS result");
        if(stage_==Stage::select){
            if(!result.keys.empty())throw std::invalid_argument("invalid selection acknowledgement");
            stage_=Stage::event;return;
        }
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
    std::pmr::string envelope(std::string_view id,std::string_view method,std::string_view params){
        return "{\"jsonrpc\":\"2.0\",\"id\":"+quote_json(id,memory_)+",\"method\":"+
            quote_json(method,memory_)+",\"params\":"+std::pmr::string(params,&memory_)+"}";
    }
    std::pmr::memory_resource& memory_;
    std::pmr::string select_id_,event_id_,select_,event_,reply_;
    Stage stage_=Stage::select;
};
} // namespace swegca::transport
