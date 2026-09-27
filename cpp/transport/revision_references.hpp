#pragma once
#include "transport/requirement_anchor.hpp"
#include "swegca_architecture/quoted_revision_kernel.hpp"

namespace swegca::transport {
// Locations are lexical evidence only. A unique spelling is not proof that the
// user meant this antecedent, nor permission to supersede its requirement.
template<class LoadPrior>
void append_revision_references(std::pmr::string& out,const Json& current,
    LoadPrior load_prior,std::pmr::memory_resource& memory,
    std::size_t limit=8,std::size_t budget=65536){
    out+=",\"revisionReferences\":[";
    std::size_t emitted=0;bool limited=false,loaded=false;
    const Json* previous=nullptr;
    const auto& items=current.at("params").at("input").values;
    for(std::size_t index=0;index<items.size()&&!limited;++index){
        if(items[index].at("type").string()!="text")continue;
        const auto text=items[index].at("text").string();
        for(std::size_t offset=0;offset<text.size();){
            const auto newline=text.find('\n',offset);
            const auto end=newline==text.npos?text.size():newline+1;
            const auto line=text.substr(offset,end-offset);
            const auto revision=architecture::kernel::quoted_revision(line);
            if(revision){
                if(emitted==limit){limited=true;break;}
                const RequirementAnchor old_quote{index,offset+revision->prior.offset,line.substr(revision->prior.offset,revision->prior.bytes)};
                const RequirementAnchor new_quote{index,offset+revision->replacement.offset,line.substr(revision->replacement.offset,revision->replacement.bytes)};
                if(!requirement_matches(old_quote,current)||!requirement_matches(new_quote,current))
                    throw std::logic_error("revision current original mismatch");
                // Bound encoding and prior reads when a proposal cannot fit.
                if(old_quote.quote.size()>budget||new_quote.quote.size()>budget){limited=true;break;}
                if(!loaded){previous=load_prior();loaded=true;}
                std::optional<RequirementAnchor> anchor;
                bool ambiguous=false;
                if(previous){
                    const auto& originals=previous->at("params").at("input").values;
                    for(std::size_t i=0;i<originals.size()&&!ambiguous;++i){
                        if(originals[i].at("type").string()!="text")continue;
                        const auto original=originals[i].at("text").string();
                        for(auto at=original.find(old_quote.quote);at!=original.npos;at=original.find(old_quote.quote,at+1)){
                            const RequirementAnchor found{i,at,old_quote.quote};
                            if(!requirement_matches(found,*previous))throw std::logic_error("revision prior original mismatch");
                            if(anchor){ambiguous=true;anchor.reset();break;}
                            anchor=found;
                        }
                    }
                }
                std::pmr::string encoded("{\"priorQuote\":",&memory);append_requirement(encoded,old_quote);
                encoded+=",\"replacementQuote\":";append_requirement(encoded,new_quote);
                encoded+=",\"priorAnchor\":";if(anchor)append_requirement(encoded,*anchor);else encoded+="null";
                encoded+=",\"locationStatus\":";
                append_json_string(encoded,!previous?"unresolved-input":ambiguous?"ambiguous":anchor?"unique":"missing");
                encoded+=",\"antecedentVerified\":false,\"replacementVerified\":false}";
                const auto separator=emitted?1U:0U;
                if(encoded.size()>budget||separator>budget-encoded.size()){limited=true;break;}
                if(separator)out+=',';
                out+=encoded;budget-=encoded.size()+separator;++emitted;
            }
            offset=end;
        }
    }
    out+="],\"revisionReferencesLimited\":";out+=limited?"true":"false";
}
} // namespace swegca::transport
