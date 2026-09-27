#pragma once
#include "transport/requirement_anchor.hpp"

namespace swegca::transport {
// Structural references into the acknowledged original, never instructions,
// extracted semantics or evidence of satisfaction. No copied text is normalized.
inline void append_input_candidates(std::pmr::string& context,const Json& native,
    const Json& acknowledged,std::pmr::memory_resource& memory,std::size_t limit,std::size_t byte_budget){
    if(!limit||context.empty()||context.back()!='}')throw std::invalid_argument("invalid candidate context");
    const auto* method=native.find("method");
    if(!method||method->kind!=Json::Kind::string||
        (method->scalar!="turn/start"&&method->scalar!="turn/steer"))return;
    const auto& items=native.at("params").at("input");
    if(items.kind!=Json::Kind::array)throw std::invalid_argument("candidate input must be an array");
    std::pmr::string references("[",&memory);std::size_t emitted=0,nontext=0;
    std::optional<std::pair<std::size_t,std::size_t>> next;
    bool oversized=false;
    for(std::size_t index=0;index<items.values.size();++index){
        const auto& item=items.values[index];
        if(item.at("type").string()!="text"){++nontext;continue;}
        const auto text=item.at("text").string();
        if(next)continue;
        std::size_t offset=0;
        while(offset<text.size()){
            const auto newline=text.find('\n',offset);
            const auto end=newline==std::string_view::npos?text.size():newline+1;
            const RequirementAnchor candidate{index,offset,text.substr(offset,end-offset)};
            if(emitted==limit){next={{index,offset}};break;}
            // Every span is checked by the existing core-backed exact-anchor
            // path. A matching span still carries no semantic verdict.
            if(!requirement_matches(candidate,native))throw std::logic_error("candidate original mismatch");
            if(candidate.quote.size()>byte_budget){next={{index,offset}};oversized=true;break;}
            std::pmr::string encoded(&memory);append_requirement(encoded,candidate);
            const auto separator=emitted?1U:0U;
            if(encoded.size()>byte_budget||separator>byte_budget-encoded.size()){
                next={{index,offset}};oversized=true;break;
            }
            if(separator)references+=',';
            references+=encoded;byte_budget-=encoded.size()+separator;++emitted;offset=end;
        }
    }
    references+=']';context.pop_back();
    if(const auto* relation=acknowledged.find("inputRelations")){
        if(context.back()!='{')context+=',';
        context+="\"inputRelations\":";append_json(context,*relation);
    }

    if(context.back()!='{')context+=',';
    context+="\"inputCandidates\":{\"inputOriginal\":";append_json(context,acknowledged.at("original"));
    context+=",\"kind\":\"uninterpreted-text-spans\",\"semanticVerified\":false,\"requirementsComplete\":false,";
    context+="\"grantsAuthority\":false,\"candidates\":";context+=references;
    context+=",\"nonTextItems\":\"";context+=std::to_string(nontext);context+="\",\"next\":";
    if(next){context+="{\"textIndex\":\"";context+=std::to_string(next->first);
        context+="\",\"byteOffset\":\"";context+=std::to_string(next->second);context+="\"}";}
    else context+="null";
    context+=",\"byteLimited\":";context+=oversized?"true":"false";context+="}}";
}
} // namespace swegca::transport
