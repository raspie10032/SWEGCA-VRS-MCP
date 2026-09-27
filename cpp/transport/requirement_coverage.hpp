#pragma once
#include "transport/input_candidates.hpp"

namespace swegca::transport {
// A presentation of authenticated source positions, never a new judgment.
// These references belong to the recalled original, not the current input.
// Only observations actually delivered in this same packet may appear here.
inline void append_recalled_requirement_coverage(std::pmr::string& context,const Json& packet,
    const Json* observations,std::pmr::memory_resource& memory,std::size_t byte_budget){
    if(context.size()<2||context.back()!='}')throw std::invalid_argument("invalid requirement coverage context");
    if(packet.at("media").string()!="application/json")return;
    Json native(&memory);
    try {native=parse_json(packet.at("content").string(),memory);}
    catch(const std::invalid_argument&){return;}
    const auto* method=native.find("method");
    if(!method||method->kind!=Json::Kind::string||
       (method->scalar!="turn/start"&&method->scalar!="turn/steer"))return;
    // Candidate construction retains its own explicit cursor and byte limit.
    // Reserve half the allowance for bounded observation links and metadata.
    std::pmr::string ack("{\"original\":",&memory);append_json(ack,packet.at("original"));ack+='}';
    std::pmr::string spans("{}",&memory);
    append_input_candidates(spans,native,parse_json(ack,memory),memory,8,byte_budget/2);
    const auto parsed=parse_json(spans,memory);
    const auto& candidates=parsed.at("inputCandidates");
    struct Binding {std::size_t observation,item,begin,bytes;};
    std::pmr::vector<Binding> bindings(&memory);
    std::pmr::string unbound("[",&memory);std::size_t inspected=0;
    const auto count=!observations?0:observations->kind==Json::Kind::array?observations->values.size():1;
    for(std::size_t index=0;index<count&&index<64;++index){
        const auto& item=observations->kind==Json::Kind::array?observations->values[index]:*observations;
        bool bound=false;
        try {
            if(item.at("media").string()=="application/json"){
                const auto report=parse_json(item.at("content").string(),memory);
                const auto& call=report.at("params").at("item");
                if(report.at("method").string()=="item/completed"&&
                   call.at("type").string()=="mcpToolCall"&&call.at("status").string()=="completed"){
                    const auto& fields=call.at("result").at("structuredContent").at("swegcaObservation");
                    const auto& original=fields.at("inputOriginal");
                    bool same=true;
                    for(const auto key:{"block","digest","offset","bytes"})
                        same=same&&original.at(key).string()==packet.at("original").at(key).string();
                    const auto* proposed=fields.find("requirement");
                    if(same&&proposed&&requirement_scope_matches(proposed,fields.at("scope").string(),memory)){
                        const auto anchor=requirement_anchor(*proposed);
                        if(requirement_matches(anchor,native)){
                            bindings.push_back({index,static_cast<std::size_t>(anchor.text_index),
                                static_cast<std::size_t>(anchor.byte_offset),anchor.quote.size()});
                            bound=true;
                        }
                    }
                }
            }
        }catch(const std::invalid_argument&){ /* No authenticated text binding. */ }
         catch(const std::out_of_range&){ /* Preserve the raw delivered observation. */ }
        if(!bound){if(unbound.size()>1)unbound+=',';append_json_string(unbound,std::to_string(index));}
        ++inspected;
    }
    unbound+=']';
    std::pmr::string output("{\"inputOriginal\":",&memory);append_json(output,packet.at("original"));
    output+=",\"kind\":\"delivered-anchor-overlap\",\"semanticVerified\":false,\"boundariesVerified\":false,\"requirementsComplete\":false,";
    output+="\"grantsAuthority\":false,\"candidates\":[";
    for(std::size_t index=0;index<candidates.at("candidates").values.size();++index){
        const auto& candidate=candidates.at("candidates").values[index];
        const auto span=requirement_anchor(candidate);
        if(index)output+=',';
        output+="{\"requirement\":";append_json(output,candidate);
        output+=",\"relatedExperienceIndices\":[";bool comma=false;
        for(const auto& binding:bindings)
            if(architecture::kernel::input_spans_overlap(span.text_index,span.byte_offset,span.quote.size(),
                binding.item,binding.begin,binding.bytes)){
                if(comma)output+=',';
                append_json_string(output,std::to_string(binding.observation));comma=true;
            }
        output+="]}";
    }
    output+="],\"unboundObservationIndices\":";output+=unbound;
    output+=",\"observationsInspected\":";append_json_string(output,std::to_string(inspected));
    output+=",\"observationsLimited\":";output+=inspected<count?"true":"false";
    output+=",\"next\":";append_json(output,candidates.at("next"));
    output+=",\"byteLimited\":";append_json(output,candidates.at("byteLimited"));
    output+=",\"nonTextItems\":";append_json(output,candidates.at("nonTextItems"));output+='}';
    // Do not truncate a source span or silently publish a partial link table.
    if(output.size()>byte_budget){
        output="{\"inputOriginal\":";append_json(output,packet.at("original"));
        output+=",\"kind\":\"delivered-anchor-overlap\",\"semanticVerified\":false,\"boundariesVerified\":false,\"requirementsComplete\":false,";
        output+="\"grantsAuthority\":false,\"candidates\":[],\"byteLimited\":true}";
    }
    context.pop_back();if(context.back()!='{')context+=',';
    context+="\"recalledRequirementCoverage\":";context+=output;context+='}';
}
} // namespace swegca::transport
