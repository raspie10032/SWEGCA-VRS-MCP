#pragma once
#include "transport/json.hpp"
#include "swegca_architecture/content_observation_kernel.hpp"
#include <charconv>
#include <cstdint>
#include <optional>
#include <stdexcept>

namespace swegca::transport {
// A producer's proposed link to an exact user-text span. It is not a semantic
// verdict: spelling/location agreement does not prove predicate relevance.
struct RequirementAnchor {
    std::uint64_t text_index, byte_offset;
    std::string_view quote;
};
inline RequirementAnchor requirement_anchor(const Json& value) {
    if(value.kind!=Json::Kind::object||value.keys.size()!=3)
        throw std::invalid_argument("requirement needs textIndex, byteOffset and quote only");
    auto number=[](const Json& field){
        const auto text=field.string();std::uint64_t result{};
        const auto parsed=std::from_chars(text.data(),text.data()+text.size(),result);
        if(text.empty()||parsed.ec!=std::errc{}||parsed.ptr!=text.data()+text.size())
            throw std::invalid_argument("invalid requirement position");
        return result;
    };
    const auto quote=value.at("quote").string();
    if(quote.empty())throw std::invalid_argument("empty requirement quote");
    return {number(value.at("textIndex")),number(value.at("byteOffset")),quote};
}
inline void append_requirement(std::pmr::string& out,const RequirementAnchor& anchor){
    out+="{\"textIndex\":\"";out+=std::to_string(anchor.text_index);
    out+="\",\"byteOffset\":\"";out+=std::to_string(anchor.byte_offset);
    out+="\",\"quote\":";append_json_string(out,anchor.quote);out+='}';
}
// A scoped connection is keyed by the entire declared scope. Its embedded
// requirement must therefore be the same claim that ingress authenticates.
// Otherwise a producer could pool different quoted requirements in one scope,
// or add evidence to an anchored scope without authenticating its anchor.
inline bool requirement_scope_matches(const Json* proposed,
    std::optional<std::string_view> scope,std::pmr::memory_resource& memory){
    if(!scope)return !proposed;
    Json declared(&memory);
    try { declared=parse_json(*scope,memory); }
    catch(const std::invalid_argument&){return !proposed;}
    const auto* embedded=declared.find("requirement");
    if(!proposed)return !embedded;
    if(!embedded)return false;
    std::pmr::string outer(&memory),inner(&memory);
    append_requirement(outer,requirement_anchor(*proposed));
    append_requirement(inner,requirement_anchor(*embedded));
    using namespace swegca::architecture::kernel;
    return to_outcome(observe_content_equality(true,true,outer==inner))==EvidenceOutcome::support;
}
inline bool requirement_matches(const RequirementAnchor& anchor,const Json& native){
    const auto* method=native.find("method");
    if(!method||method->kind!=Json::Kind::string||
        (method->scalar!="turn/start"&&method->scalar!="turn/steer"))return false;
    const auto& items=native.at("params").at("input");
    if(items.kind!=Json::Kind::array||anchor.text_index>=items.values.size())return false;
    // Index is the original input array position, including non-text items.
    const auto& item=items.values[anchor.text_index];
    if(item.at("type").string()!="text")return false;
    const auto text=item.at("text").string();
    const bool complete=anchor.byte_offset<=text.size()&&anchor.quote.size()<=text.size()-anchor.byte_offset;
    using namespace swegca::architecture::kernel;
    return to_outcome(observe_content_equality(complete,true,
        complete&&text.substr(anchor.byte_offset,anchor.quote.size())==anchor.quote))==EvidenceOutcome::support;
}
} // namespace swegca::transport
