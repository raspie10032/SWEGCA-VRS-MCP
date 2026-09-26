#include "transport/json.hpp"
#include "transport/resource_profile.hpp"
#include "vrs/runtime.hpp"
#include <charconv>
#include <fstream>
#include <iostream>
#include <limits>

using namespace swegca::transport;
using namespace swegca::vrs;
using namespace swegca::architecture;
namespace {
std::uint64_t number(std::string_view text){std::uint64_t n;auto r=std::from_chars(text.data(),text.data()+text.size(),n);if(r.ec!=std::errc{}||r.ptr!=text.data()+text.size()||text.empty())throw std::invalid_argument("expected unsigned decimal string");return n;}
std::uint64_t integer(const Json& j){return number(j.string());}
double real(const Json& j){if(j.kind!=Json::Kind::number)throw std::invalid_argument("expected numeric policy value");double d;auto r=std::from_chars(j.scalar.data(),j.scalar.data()+j.scalar.size(),d);if(r.ec!=std::errc{}||r.ptr!=j.scalar.data()+j.scalar.size())throw std::invalid_argument("invalid policy number");return d;}
DigestBytes digest(std::string_view text){
    if(text.size()!=64)throw std::invalid_argument("identity requires 64 lowercase hex digits");
    DigestBytes d{};
    const auto hex=[](char c)->unsigned{if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;throw std::invalid_argument("invalid identity hex");};
    for(std::size_t i=0;i<32;++i)d[i]=std::byte((hex(text[2*i])<<4)|hex(text[2*i+1]));
    return d;
}
std::pmr::string hex(std::span<const std::byte> data,MemoryBudget& memory){constexpr char digits[]="0123456789abcdef";std::pmr::string s(&memory);for(auto v:data){unsigned n=std::to_integer<unsigned>(v);s+=digits[n>>4];s+=digits[n&15];}return s;}
std::pmr::string address(const ExperienceLocation& a,MemoryBudget& m){return "{\"block\":\""+hex(a.block,m)+"\",\"offset\":\""+std::to_string(a.offset).c_str()+"\",\"bytes\":\""+std::to_string(a.bytes).c_str()+"\",\"digest\":\""+hex(a.digest,m)+"\"}";}
std::pmr::string decimal(double value,MemoryBudget& memory){
    std::array<char,64> buffer{};auto result=std::to_chars(buffer.data(),buffer.data()+buffer.size(),value,std::chars_format::general,std::numeric_limits<double>::max_digits10);
    if(result.ec!=std::errc{})throw std::runtime_error("cannot encode numeric result");
    return std::pmr::string(buffer.data(),result.ptr,&memory);
}
std::pmr::string refinement(const ConnectionRefinement& report,MemoryBudget& memory){
    const auto& judgment=report.result().verification().judgment();
    return std::pmr::string("{\"status\":",&memory)+std::to_string(static_cast<unsigned>(judgment.status())).c_str()+
        ",\"reason\":"+std::to_string(static_cast<unsigned>(judgment.reason())).c_str()+
        ",\"strength\":"+decimal(report.result().strength().current(),memory)+
        ",\"revision\":\""+std::to_string(report.after_revision()).c_str()+"\"}";
}
std::pmr::string read_frame(std::istream& input,std::size_t limit,MemoryBudget& memory,bool& eof){
    std::pmr::string line(&memory);bool overflow=false;char c;
    while(input.get(c)){if(c=='\n'){if(overflow)throw std::length_error("frame exceeds configured limit");return line;}if(line.size()==limit)overflow=true;if(!overflow)line+=c;}
    eof=true;if(overflow||!line.empty())throw std::invalid_argument("truncated MCP frame");return line;
}
constexpr std::string_view tools_list=R"({"tools":[
{"name":"vrs_replay","description":"Read one original from current Recall. Optional offset and count return only that verified byte range, without a completed Replay receipt for Re-evidence. Does not infer truth or authorize actions.","inputSchema":{"type":"object","properties":{"receipt":{"type":"string"},"candidate":{"type":"string"},"offset":{"type":"string","description":"Raw payload byte offset; requires count."},"count":{"type":"string","description":"Byte count; requires offset."}},"required":["receipt","candidate"],"additionalProperties":false}},
{"name":"vrs_re_evidence","description":"Use SWEGCA to check recorded current observations after the selected Replay.","inputSchema":{"type":"object","properties":{"receipt":{"type":"string"},"seed":{"type":"string"},"step":{"type":"string"}},"required":["receipt","seed","step"],"additionalProperties":false}}
]})";
class Server {
public:
    Server(Runtime& runtime,MemoryBudget& memory):runtime_(runtime),memory_(memory){}
    void message(std::string_view line){
        Json request(&memory_);
        try{request=parse_json(line,memory_);}catch(const std::bad_alloc&){throw;}catch(const std::exception&){error("null",-32700,"invalid JSON");return;}
        if(request.kind!=Json::Kind::object||!request.find("jsonrpc")||request.at("jsonrpc").kind!=Json::Kind::string||request.at("jsonrpc").scalar!="2.0"||!request.find("method")||request.at("method").kind!=Json::Kind::string){error("null",-32600,"invalid JSON-RPC request");return;}
        const Json* id=request.find("id");std::pmr::string encoded_id("null",&memory_);
        if(id&&(id->kind==Json::Kind::string||id->kind==Json::Kind::number))encoded_id=encode_json(*id,memory_);
        else if(id){error("null",-32600,"invalid request id");return;}
        try{
            if(request.at("jsonrpc").string()!="2.0")throw std::invalid_argument("invalid JSON-RPC version");
            const auto method=request.at("method").string();
            if(!id){if(method=="notifications/initialized"&&initialized_)ready_=true;return;}
            if(method=="initialize"){
                if(initialized_)throw std::invalid_argument("already initialized");
                const auto& p=request.at("params");(void)p.at("protocolVersion").string();
                if(p.at("capabilities").kind!=Json::Kind::object||p.at("clientInfo").kind!=Json::Kind::object)throw std::invalid_argument("invalid initialize parameters");
                initialized_=true;result(encoded_id,R"({"protocolVersion":"2025-06-18","capabilities":{"tools":{},"experimental":{"swegcaHostInput":{"version":"4"}}},"serverInfo":{"name":"swegca-vrs-cpp","version":"0.1"}})");return;
            }
            if(method=="ping"){result(encoded_id,"{}");return;}
            if(!ready_)throw std::invalid_argument("initialization not completed");
            if(method=="tools/list"){result(encoded_id,encode_json(parse_json(tools_list,memory_),memory_));return;}
            if(method=="tools/call"){
                const auto& p=request.at("params");
                const auto name=p.at("name").string();
                if(name!="vrs_replay"&&name!="vrs_re_evidence")throw std::invalid_argument("unknown tool");
                try{auto body=call(name,p.at("arguments"));
                    result(encoded_id,"{\"content\":[{\"type\":\"text\",\"text\":"+quote_json(body,memory_)+"}],\"structuredContent\":"+body+"}");
                }catch(const std::exception& e){result(encoded_id,"{\"content\":[{\"type\":\"text\",\"text\":"+quote_json(e.what(),memory_)+"}],\"isError\":true}");}return;
            }
            if(method.starts_with("swegca/")){auto body=host(method,request.at("params"));result(encoded_id,body);return;}
            error(encoded_id,-32601,"unknown method");
        }catch(const std::invalid_argument& e){if(id)error(encoded_id,-32602,e.what());}
        catch(const std::exception& e){if(id)error(encoded_id,-32000,e.what());}
    }
    void framing_error(){error("null",-32700,"invalid or oversized MCP frame");}
private:
    Runtime& runtime_;MemoryBudget& memory_;bool initialized_=false,ready_=false;
    std::uint64_t receipt_=0;
    std::optional<ReceivedInput> received_;
    std::optional<ReplayedInput> replayed_;
    void result(std::string_view id,std::string_view body){std::cout<<"{\"jsonrpc\":\"2.0\",\"id\":"<<id<<",\"result\":"<<body<<"}\n"<<std::flush;if(!std::cout)throw std::runtime_error("MCP output disconnected");}
    void error(std::string_view id,int code,std::string_view message){std::cout<<"{\"jsonrpc\":\"2.0\",\"id\":"<<id<<",\"error\":{\"code\":"<<code<<",\"message\":"<<quote_json(message,memory_)<<"}}\n"<<std::flush;}
    void clear(){replayed_.reset();received_.reset();}
    static std::size_t candidate_limit(const Json& p) {
        const auto* value=p.find("candidateLimit");
        const auto limit=value?integer(*value):64;
        if(!limit||limit>256)throw std::invalid_argument("candidateLimit must be 1..256");
        return static_cast<std::size_t>(limit);
    }
    void candidate_page(std::pmr::string& body,std::uint64_t offset,std::size_t limit) const {
        const auto matches=received_->recalled.matches();
        if(offset>matches.size())throw std::invalid_argument("candidate offset exceeds receipt");
        const auto begin=static_cast<std::size_t>(offset);
        const auto end=begin+std::min(limit,matches.size()-begin);
        body+="\"candidateCount\":\"";body+=std::to_string(matches.size());body+="\",\"nextOffset\":";
        if(end<matches.size()){body+='"';body+=std::to_string(end);body+='"';}else body+="null";
        body+=",\"candidates\":[";
        for(auto index=begin;index<end;++index){
            if(index!=begin)body+=',';
            body+="{\"index\":\"";body+=std::to_string(index);body+="\",\"original\":";
            body+=address(matches[index].original,memory_);body+='}';
        }
        body+=']';
    }
    std::pmr::string host(std::string_view method,const Json& p){
        if(method=="swegca/candidates"){
            if(!received_||integer(p.at("receipt"))!=receipt_)throw std::invalid_argument("expired receipt");
            const auto limit=candidate_limit(p);
            std::pmr::string body("{\"receipt\":\"",&memory_);body+=std::to_string(receipt_);body+="\",";
            candidate_page(body,integer(p.at("offset")),limit);body+='}';return body;
        }
        if(method=="swegca/start"){runtime_.start_session(digest(p.at("identity").string()),p.at("name").string());return std::pmr::string("{}",&memory_);}
        if(method=="swegca/resume"){runtime_.resume_session(digest(p.at("identity").string()));return std::pmr::string("{}",&memory_);}
        if(method=="swegca/end"){clear();runtime_.end_session();return std::pmr::string("{}",&memory_);}
        if(method=="swegca/work"){const auto count=runtime_.work(integer(p.at("seed")),integer(p.at("step")));return std::pmr::string("{\"merged\":\"",&memory_)+std::to_string(count).c_str()+"\"}";}
        if(method=="swegca/work/start"){
            const bool started=runtime_.schedule_work(integer(p.at("seed")),integer(p.at("step")));
            return std::pmr::string(started?"{\"started\":true}":"{\"started\":false}",&memory_);
        }
        if(method=="swegca/work/poll"){
            const auto count=runtime_.poll_work();
            if(!count)return std::pmr::string("{\"running\":true,\"merged\":null}",&memory_);
            return std::pmr::string("{\"running\":false,\"merged\":\"",&memory_)+std::to_string(*count).c_str()+"\"}";
        }
        if(method=="swegca/define"){runtime_.define_connection(digest(p.at("identity").string()));return std::pmr::string("{}",&memory_);}
        if(method=="swegca/receive"||method=="swegca/observe"||method=="swegca/retain"){
            const auto page_limit=method=="swegca/receive"?candidate_limit(p):64;
            const auto sequence=integer(p.at("sequence")),observed=integer(p.at("observedAt")),seed=integer(p.at("seed")),step=integer(p.at("step"));
            const auto session=p.at("session").string(),source=p.at("source").string(),media=p.at("media").string();
            std::pmr::vector<std::byte> binary(&memory_);std::span<const std::byte> content;
            const auto* text=p.find("content");const auto* encoded=p.find("contentHex");
            if(bool(text)==bool(encoded))throw std::invalid_argument("exactly one content encoding is required");
            if(text){const auto value=text->string();content=std::as_bytes(std::span(value));}
            else {const auto value=encoded->string();if(value.size()%2)throw std::invalid_argument("invalid content hex");
                for(std::size_t i=0;i<value.size();i+=2){unsigned byte=0;auto r=std::from_chars(value.data()+i,value.data()+i+2,byte,16);if(r.ec!=std::errc{}||r.ptr!=value.data()+i+2)throw std::invalid_argument("invalid content hex");binary.push_back(std::byte(byte));}content=binary;}
            if(method=="swegca/retain"){
                // Session events use the same SWEGCA admission, shuffle and
                // refinement path without replacing the current input/Replay.
                auto recorded=runtime_.retain({sequence,observed,session,source,media,content},seed,step);
                return "{\"original\":"+address(recorded.original,memory_)+",\"refinement\":"+refinement(recorded.refinement,memory_)+"}";
            }
            if(method=="swegca/observe"){
                using namespace swegca::architecture::kernel;
                const auto& fields=p.at("observation");EvidenceObservation value;
                value.hypothesis=digest(fields.at("hypothesis").string());
                value.source=digest(fields.at("source").string());value.context=digest(fields.at("context").string());value.producer=digest(fields.at("producer").string());
                value.observed_at=observed;value.expires_at=integer(fields.at("expiresAt"));value.producer_confidence=real(fields.at("confidence"));
                const auto axis=integer(fields.at("axis"));if(axis>UINT32_MAX)throw std::invalid_argument("axis overflow");value.axis=static_cast<std::uint32_t>(axis);
                const auto& expiry=fields.at("hasExpiry");if(expiry.kind!=Json::Kind::boolean)throw std::invalid_argument("hasExpiry must be boolean");value.has_expiry=expiry.scalar=="true";
                const auto outcome=fields.at("outcome").string();
                if(outcome=="support")value.outcome=EvidenceOutcome::support;
                else if(outcome=="refute")value.outcome=EvidenceOutcome::refute;
                else if(outcome=="insufficient")value.outcome=EvidenceOutcome::insufficient;
                else throw std::invalid_argument("unknown observed outcome");
                // This is the host's recorded observation, never a caller-supplied
                // SWEGCA verdict. Preserve the prior selected Replay for comparison.
                auto recorded=runtime_.observe(value.hypothesis,{sequence,observed,session,source,media,content},value,seed,step);
                return "{\"original\":"+address(recorded.original,memory_)+",\"refinement\":"+refinement(recorded.refinement,memory_)+"}";
            }
            if(receipt_==std::numeric_limits<std::uint64_t>::max())throw std::overflow_error("receipt sequence exhausted");
            clear();++receipt_;received_.emplace(runtime_.receive({sequence,observed,session,source,media,content},seed,step));
            std::pmr::string body("{\"receipt\":\"",&memory_);body+=std::to_string(receipt_);body+="\",\"original\":";body+=address(received_->recorded.original,memory_);
            body+=",\"temporary\":";body+=received_->recalled.temporary()?"true":"false";body+=',';
            candidate_page(body,0,page_limit);
            body+=",\"refinement\":";body+=refinement(received_->recorded.refinement,memory_);body+='}';return body;
        }
        throw std::invalid_argument("unknown SWEGCA host method");
    }
    std::pmr::string call(std::string_view method,const Json& p){
        if(!received_||integer(p.at("receipt"))!=receipt_)throw std::invalid_argument("expired receipt");
        if(method=="vrs_replay"){
            const auto candidate=integer(p.at("candidate"));replayed_.reset();
            if(p.find("offset") || p.find("count")){
                const auto offset=integer(p.at("offset")),count=integer(p.at("count"));
                auto part=runtime_.read_payload_slice(received_->recalled,candidate,offset,count);
                std::pmr::string body("{\"original\":",&memory_);
                body+=address(part.evidence().original(),memory_);
                body+=",\"partial\":true,\"offset\":\"";body+=std::to_string(part.offset());
                body+="\",\"totalBytes\":\"";body+=std::to_string(part.total_bytes());
                body+="\",\"contentHex\":\"";body+=hex(part.content(),memory_);body+="\"}";
                return body;
            }
            replayed_.emplace(runtime_.replay(received_->recalled,candidate));
            const auto value=evidence_payload(replayed_->original());
            return "{\"original\":"+address(replayed_->location(),memory_)+",\"media\":"+quote_json(value.media_type,memory_)+",\"source\":"+quote_json(value.source,memory_)+",\"contentHex\":\""+hex(value.content,memory_)+"\"}";
        }
        if(method=="vrs_re_evidence"){
            if(!replayed_)throw std::invalid_argument("Replay required before Re-evidence");
            auto checked=runtime_.re_evidence(*replayed_,integer(p.at("seed")),integer(p.at("step")));
            auto body=std::pmr::string("{\"agreement\":",&memory_)+std::to_string(static_cast<unsigned>(checked.agreement())).c_str()+",\"status\":"+std::to_string(static_cast<unsigned>(checked.verification().result().verification().judgment().status())).c_str();
            body+=",\"replayedOriginal\":";body+=address(checked.replayed_original(),memory_);
            body+=",\"rememberedHead\":";body+=address(checked.remembered_head().record,memory_);
            body+=",\"currentHead\":";body+=address(checked.current_head().record,memory_);
            body+=",\"currentOriginals\":[";bool first=true;
            for(const auto& original:checked.current_originals()){if(!first)body+=',';first=false;body+=address(original,memory_);}
            body+="]}";return body;
        }
        throw std::invalid_argument("unknown tool");
    }
};
}
int main(int argc,char** argv){
    try{
        if(argc!=4)throw std::invalid_argument("usage: swegca-vrs-mcp create|open ROOT CONFIG.json");
        std::string_view mode=argv[1];
        const bool limited=mode.starts_with("limited-"),bounded=mode.starts_with("bounded-");
        if(limited||bounded)mode.remove_prefix(8);
        if(mode!="create"&&mode!="open")throw std::invalid_argument("mode must be create/open or limited-create/limited-open");
        MemoryBudget config_memory(1<<20);std::ifstream file(argv[3]);if(!file)throw std::runtime_error("cannot open configuration");
        std::pmr::string text(&config_memory);char c;while(file.get(c)){if(text.size()==65536)throw std::length_error("configuration too large");text+=c;}
        auto config=parse_json(text,config_memory);const auto ram=integer(config.at("memoryBytes"));const auto frame=integer(config.at("frameBytes"));
        if(!ram||!frame||frame>ram)throw std::invalid_argument("invalid resource limits");
        if(limited)launch_resource_profile(mode,argv[2],argv[3],ram,config.at("cpuAffinity").string());
        if(bounded)verify_resource_profile(ram,config.at("cpuAffinity").string());
        MemoryBudget memory(ram);
        EvidencePolicy policy;const auto& p=config.at("policy");
#define REAL(field) policy.field=real(p.at(#field))
#define UINT(field) {auto n=integer(p.at(#field));if(n>UINT32_MAX)throw std::invalid_argument("policy integer overflow");policy.field=static_cast<std::uint32_t>(n);}
        REAL(chance_rate);REAL(accept_margin);REAL(confidence_level);REAL(prior_alpha);REAL(prior_beta);REAL(regime_change_threshold);
        UINT(minimum_effective_samples_per_axis);UINT(minimum_source_diversity);UINT(minimum_axis_source_diversity);UINT(minimum_context_diversity);UINT(recent_window);UINT(minimum_recent_samples);UINT(axis_count);
#undef REAL
#undef UINT
        RuntimeConfig settings{digest(config.at("mainIdentity").string()),policy,real(config.at("initialStrength")),integer(config.at("sessionBlockBytes")),integer(config.at("mainBlockBytes")),integer(config.at("readLimit"))};
        const auto workers=integer(config.at("mergeWorkers"));
        if(!workers||workers>UINT32_MAX)throw std::invalid_argument("invalid merge worker count");
        settings.io_bytes_per_second=integer(config.at("ioBytesPerSecond"));
        settings.storage_bytes=integer(config.at("storageBytes"));
        settings.merge_workers=static_cast<std::uint32_t>(workers);
        auto runtime=mode=="create"?Runtime::create(argv[2],settings,memory):Runtime::open(argv[2],settings,memory);
        Server server(runtime,memory);bool eof=false;
        while(!eof){try{auto line=read_frame(std::cin,frame,memory,eof);if(!eof)server.message(line);}catch(const std::bad_alloc&){std::cerr<<"VRS memory budget exhausted\n";return 2;}catch(const std::exception&){server.framing_error();}if(!std::cout)return 2;}
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
