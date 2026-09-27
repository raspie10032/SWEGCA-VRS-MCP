#include "transport/agent_query_socket.hpp"
#include "transport/requirement_anchor.hpp"
#include "transport/json.hpp"
#include "transport/stdio_frames.hpp"
#include "transport/ingress_probe.hpp"
#include "transport/agent_event.hpp"
#include "transport/app_server_requests.hpp"
#include "swegca_architecture/input_cue.hpp"
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
ExperienceLocation record_address(const Json& value){
    return {digest(value.at("block").string()),integer(value.at("offset")),integer(value.at("bytes")),digest(value.at("digest").string())};
}
std::string_view content_text(const StoredExperience& record){
    const auto bytes=record.view().content;return {reinterpret_cast<const char*>(bytes.data()),bytes.size()};
}
std::pmr::string decimal(double value,MemoryBudget& memory){
    std::array<char,64> buffer{};auto result=std::to_chars(buffer.data(),buffer.data()+buffer.size(),value,std::chars_format::general,std::numeric_limits<double>::max_digits10);
    if(result.ec!=std::errc{})throw std::runtime_error("cannot encode numeric result");
    return std::pmr::string(buffer.data(),result.ptr,&memory);
}
std::pmr::string refinement(const ConnectionRefinement& report,MemoryBudget& memory){
    const auto& judgment=report.result().verification().judgment();
    return std::pmr::string("{\"connection\":\"",&memory)+hex(report.connection(),memory)+"\",\"status\":"+std::to_string(static_cast<unsigned>(judgment.status())).c_str()+
        ",\"reason\":"+std::to_string(static_cast<unsigned>(judgment.reason())).c_str()+
        ",\"strength\":"+decimal(report.result().strength().current(),memory)+
        ",\"revision\":\""+std::to_string(report.after_revision()).c_str()+"\"}";
}
constexpr std::string_view tools_list=R"({"tools":[
{"name":"vrs_replay","description":"Read one original from current Recall. Without candidate, SWEGCA selects by stored connection strength, recency and stable address. Optional offset/count return verified partial bytes. Scope performs a separate complete scoped Recall/Replay/comparison; it does not replace the parent cognition or the parent Replay used by vrs_re_evidence. Does not infer truth or authorize actions.","inputSchema":{"type":"object","properties":{"receipt":{"type":"string"},"inputOriginal":{"type":"object","description":"Expected current input address paired with receipt. Rejects receipt reuse for a different input.","properties":{"block":{"type":"string"},"offset":{"type":"string"},"bytes":{"type":"string"},"digest":{"type":"string"}},"required":["block","offset","bytes","digest"],"additionalProperties":false},"connection":{"type":"string","description":"Replay this recorded related connection using core selection and its own comparison journal. Requires related=true and inputOriginal; cannot combine with connections listing."},"connections":{"type":"object","description":"List core-selected originals per recorded connection, without Replay or whole-purpose verdict. Requires related=true and inputOriginal. Next page requires returned snapshot.","properties":{"limit":{"type":"string"},"after":{"type":"string"},"snapshot":{"type":"string"}},"required":["limit"],"additionalProperties":false},"candidate":{"type":"string"},"related":{"type":"boolean","description":"Follow recorded observation links from the automatically selected parent Replay. Separate comparison; cannot combine with scope or candidate/range."},"scope":{"type":"string","minLength":1,"description":"Exact producer-declared scope under the current input. Cannot combine with candidate, offset or count. A miss never returns unrelated dialogue."},"offset":{"type":"string","description":"Raw payload byte offset; requires count."},"count":{"type":"string","description":"Byte count; requires offset."}},"required":["receipt"],"additionalProperties":false}},
{"name":"vrs_re_evidence","description":"Compare the selected Replay with recorded current observations through SWEGCA; run Re-evidence only on a verified conflict.","inputSchema":{"type":"object","properties":{"receipt":{"type":"string"},"seed":{"type":"string"},"step":{"type":"string"}},"required":["receipt","seed","step"],"additionalProperties":false}}
]})";
class Server {
public:
    using AutomaticWork=std::optional<std::pair<std::uint64_t,std::uint64_t>>;
    Server(Runtime& runtime,MemoryBudget& memory,std::uint64_t frame,AutomaticWork automatic={})
        :runtime_(runtime),memory_(memory),frame_(frame),automatic_(automatic),work_requested_(automatic.has_value()),contexts_(&memory){}
    [[nodiscard]] bool automatic_work() const noexcept{return true;}
    bool advance_work(){
        if(!ready_)return false;
        if(automatic_&&work_requested_){
            if(!runtime_.work_scheduled())(void)runtime_.schedule_work(automatic_->first,automatic_->second);
            work_requested_=false;
        }
        if(automatic_&&runtime_.work_scheduled()){
            const auto before=runtime_.main().head();
            (void)runtime_.poll_work();
            if(before!=runtime_.main().head())invalidate_main_cognition();
        }
        if(runtime_.work_scheduled())return automatic_.has_value();
        return runtime_.maintain_memory();
    }
    void message(std::string_view line){
        SWEGCA_INGRESS_STAGE("host_frame");
        Json request(&memory_);
        JsonMemberSource native_source;
        constexpr std::string_view native_path[]{"params","native"};
        try{request=parse_json_member(line,memory_,native_path,native_source);}catch(const std::bad_alloc&){throw;}catch(const std::exception&){error("null",-32700,"invalid JSON");return;}
        SWEGCA_INGRESS_STAGE("host_rpc_parsed");
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
                initialized_=true;
                auto body=std::pmr::string(R"({"protocolVersion":"2025-06-18","capabilities":{"tools":{},"experimental":{"swegcaHostInput":{"version":"15","frameBytes":")",&memory_);
                body+=std::to_string(frame_);body+=R"("}}},"serverInfo":{"name":"swegca-vrs-cpp","version":"0.1"}})";
                result(encoded_id,body);return;
            }
            if(method=="ping"){result(encoded_id,"{}");return;}
            if(!ready_)throw std::invalid_argument("initialization not completed");
            if(method=="tools/list"){result(encoded_id,encode_json(parse_json(tools_list,memory_),memory_));return;}
            if(method=="tools/call"){
                const auto& p=request.at("params");
                const auto name=p.at("name").string();
                if(name!="vrs_replay"&&name!="vrs_re_evidence")throw std::invalid_argument("unknown tool");
                try{
                    if(name=="vrs_replay")replay_result(encoded_id,p.at("arguments"));
                    else {auto body=call(name,p.at("arguments"));tool_result(encoded_id,body);}
                }catch(const std::exception& e){result(encoded_id,"{\"content\":[{\"type\":\"text\",\"text\":"+quote_json(e.what(),memory_)+"}],\"isError\":true}");}return;
            }
            if(method=="swegca/agent/replay"){
                agent_replay_result(encoded_id,request.at("params"));return;
            }
            if(method.starts_with("swegca/")){auto body=host(method,request.at("params"),line,native_source);result(encoded_id,body);return;}
            error(encoded_id,-32601,"unknown method");
        }catch(const std::invalid_argument& e){if(id)error(encoded_id,-32602,e.what());}
        catch(const std::exception& e){if(id)error(encoded_id,-32000,e.what());}
    }
    void framing_error(){error("null",-32700,"invalid or oversized MCP frame");}
    std::pmr::string agent_query(std::string_view line){
        // This listener accepts only the narrow owner query, never initialize,
        // raw host mutations, session management or evidence producers.
        const auto request=parse_json(line,memory_);
        if(request.at("method").string()!="swegca/agent/replay")
            throw std::invalid_argument("query method unavailable");
        class Buffer final:public std::streambuf {
        public:
            Buffer(MemoryBudget& memory,std::size_t limit):bytes(&memory),limit_(limit){}
            std::pmr::string bytes;
        protected:
            std::streamsize xsputn(const char* data,std::streamsize count) override{
                if(count<0||static_cast<std::size_t>(count)>limit_-bytes.size())
                    throw std::length_error("query reply exceeds frame limit");
                bytes.append(data,static_cast<std::size_t>(count));return count;
            }
            int_type overflow(int_type value) override{
                if(traits_type::eq_int_type(value,traits_type::eof()))return traits_type::not_eof(value);
                const char byte=traits_type::to_char_type(value);xsputn(&byte,1);return value;
            }
        private:std::size_t limit_;
        } buffer(memory_,frame_);
        std::ostream stream(&buffer);stream.exceptions(std::ios::badbit|std::ios::failbit);
        auto* previous=output_;output_=&stream;
        try{message(line);}catch(...){output_=previous;throw;}
        output_=previous;return std::move(buffer.bytes);
    }
private:
    std::ostream* output_=&std::cout;
    Runtime& runtime_;MemoryBudget& memory_;std::uint64_t frame_;bool initialized_=false,ready_=false;
    AutomaticWork automatic_;
    bool work_requested_=false;
    struct Context {
        struct ScopedCognition {
            ScopedCognition(const ExperienceLocation& input_value,std::string_view name,InputCognition value,std::pair<std::uint64_t,std::uint64_t> params,MemoryBudget& memory)
                :input(input_value),scope(name,&memory),cognition(std::move(value)),parameters(params){}
            ExperienceLocation input;
            std::pmr::string scope;
            std::optional<InputCognition> cognition;
            std::pair<std::uint64_t,std::uint64_t> parameters;
            bool dirty=false,temporary=false,saved=false;
            std::optional<DigestBytes> revision;
        };
        struct RestoredCognition {
            RestoredCognition(ExperienceLocation original,InputCognition value,std::pair<std::uint64_t,std::uint64_t> params,
                std::string_view checkpoint,MemoryBudget& memory)
                :input(original),cognition(std::move(value)),parameters(params),recovery(checkpoint,&memory){}
            ExperienceLocation input;
            std::optional<InputCognition> cognition;
            std::pair<std::uint64_t,std::uint64_t> parameters;
            std::pmr::string recovery;
            std::optional<ExperienceLocation> related_from;
            std::optional<DigestBytes> related_connection;
            bool dirty=false,saved=false;
            std::optional<DigestBytes> revision;
        };
        struct Delivery { DigestBytes fingerprint; ExperienceLocation original; DigestBytes context{}; ExperienceSender sender=ExperienceSender::unspecified; DigestBytes connection{}; };
        Context(MemoryBudget& memory,std::string_view native,bool app,bool connection):deliveries(&memory),turn_inputs(&memory),native_session(native,&memory),app_server(app),connection_scope(connection){}
        std::pmr::map<std::uint64_t,Delivery> deliveries;
        std::pmr::map<std::pmr::string,std::pmr::vector<Delivery>,std::less<>> turn_inputs;
        std::uint64_t indexed_turn_deliveries=0;
        bool native_ready=false;
        std::pmr::string native_session;
        bool app_server=false;
        bool connection_scope=false;
        std::string_view native_source() const noexcept{return connection_scope?"codex/app-server-connection":app_server?"codex/app-server":"codex/hook";}
        std::uint64_t receipt=0;
        std::optional<ReceivedInput> received;
        std::optional<ReplayedInput> replayed;
        std::optional<InputCognition> cognition;
        std::optional<ScopedCognition> scoped;
        std::optional<RestoredCognition> restored,related;
        // Default core-selected connection survives disposal of Replay bytes.
        // This identity is bookkeeping, never a substitute for a full Replay.
        std::optional<DigestBytes> cognition_connection;
        bool cognition_done=false,cognition_saved=false;
        bool cognition_snapshot_saved=false;
        std::optional<DigestBytes> cognition_revision;
        std::optional<std::pair<std::uint64_t,std::uint64_t>> cognition_parameters;
        std::optional<StoredExperience> recovered_cognition;
        std::uint64_t recovered_receipt=0;
    };
    std::uint64_t next_receipt_=0;
    std::pmr::map<DigestBytes,Context> contexts_;
    Context* selected_=nullptr;
    Context& context() const {
        if(!selected_)throw std::invalid_argument("no selected session");
        return *selected_;
    }
    // Narrow owner-side query entry for agent adapters. The caller cannot
    // select a session, inject evidence, end a session or drive Main work.
    // Resolve the live receipt together with its sealed input before changing
    // selection, then restore the transport owner's selection on every exit.
    void agent_replay_result(std::string_view id,const Json& p){
        if(p.kind!=Json::Kind::object)throw std::invalid_argument("expected replay arguments");
        for(const auto& key:p.keys)
            if(key!="receipt"&&key!="inputOriginal"&&key!="scope"&&key!="related"&&key!="connections"&&key!="connection")
                throw std::invalid_argument("unsupported agent replay argument");
        const auto receipt=integer(p.at("receipt"));
        const auto input=record_address(p.at("inputOriginal"));
        if(!receipt)throw std::invalid_argument("invalid agent receipt");
        auto previous=contexts_.end(),target=contexts_.end();
        for(auto it=contexts_.begin();it!=contexts_.end();++it){
            auto& state=it->second;
            if(&state==selected_)previous=it;
            if(state.native_session.empty()||!state.native_ready)continue;
            if(state.recovered_cognition&&state.recovered_receipt==receipt){
                const auto metadata=parse_json(content_text(*state.recovered_cognition),memory_);
                if(record_address(metadata.at("inputOriginal"))==input)target=it;
            }else if(state.received&&state.receipt==receipt&&state.received->recorded.original==input){
                target=it;
            }
        }
        if(target==contexts_.end())throw std::invalid_argument("agent replay input or receipt unavailable");
        if(previous==contexts_.end())throw std::invalid_argument("no active transport selection");
        select_context(target->first);
        try{replay_result(id,p);}
        catch(...){select_context(previous->first);throw;}
        select_context(previous->first);
    }
    // Derive turn provenance only from authenticated request/response pairs.
    // This runs for output notifications, never before input Recall. The cache
    // is rebuilt from originals after restart; it is not a new evidence source.
    std::pmr::string turn_key(std::string_view thread,std::string_view turn){
        std::pmr::string key(std::to_string(thread.size()),&memory_);key+=':';key+=thread;key+=turn;return key;
    }
    std::optional<std::pmr::string> acknowledged_turn_key(const AgentEvent& request,const Json& reply){
        if(reply.find("method"))return std::nullopt;
        const auto* result=reply.find("result");
        if(!result||result->kind!=Json::Kind::object)return std::nullopt;
        const auto method=request.native_name();
        const Json* id=nullptr;
        if(method=="turn/start"){
            const auto* turn=result->find("turn");
            if(turn&&turn->kind==Json::Kind::object)id=turn->find("id");
        }else if(method=="turn/steer")id=result->find("turnId");
        if(!id||id->kind!=Json::Kind::string||id->scalar.empty())return std::nullopt;
        const auto& params=request.fields().at("params");
        if(method=="turn/steer"){
            const auto* expected=params.find("expectedTurnId");
            if(!expected||expected->kind!=Json::Kind::string||expected->scalar!=id->scalar)return std::nullopt;
        }
        return turn_key(params.at("threadId").string(),id->scalar);
    }
    void remember_turn_input(Context& state,std::string_view key,const Context::Delivery& input){
        auto& inputs=state.turn_inputs.try_emplace(std::pmr::string(key,&memory_)).first->second;
        const auto duplicate=std::find_if(inputs.begin(),inputs.end(),[&](const auto& prior){return prior.original==input.original;});
        if(duplicate==inputs.end())inputs.push_back(input);
    }
    std::optional<Context::Delivery> turn_input(Context& state,std::string_view thread,std::string_view turn,std::optional<ExperienceLocation> requested){
        for(auto it=state.deliveries.lower_bound(state.indexed_turn_deliveries);it!=state.deliveries.end();++it){
            const auto& delivered=it->second;
            if(delivered.sender==ExperienceSender::server){
                const auto stored=runtime_.session().read_original(delivered.original);
                const auto payload=evidence_payload(stored);
                const std::string_view bytes(reinterpret_cast<const char*>(payload.content.data()),payload.content.size());
                const auto fields=parse_json(bytes,memory_);
                const auto* result=fields.find("result");
                const auto* returned_turn=result&&result->kind==Json::Kind::object?result->find("turn"):nullptr;
                const auto* id=returned_turn&&returned_turn->kind==Json::Kind::object?returned_turn->find("id"):nullptr;
                const auto* steer_id=result&&result->kind==Json::Kind::object?result->find("turnId"):nullptr;
                if(!fields.find("method")&&(id||steer_id)){
                    for(auto prior=it;prior!=state.deliveries.begin();){
                        --prior;
                        const auto& input=prior->second;
                        if(input.original.digest!=delivered.context||input.connection!=delivered.connection||
                            input.sender!=ExperienceSender::client)continue;
                        const auto request=runtime_.session().read_original(input.original);
                        const auto request_payload=evidence_payload(request);
                        const std::string_view request_bytes(reinterpret_cast<const char*>(request_payload.content.data()),request_payload.content.size());
                        auto event=state.connection_scope?adapt_codex_app_server_connection(request_bytes,state.native_session,memory_):
                            adapt_codex_app_server(request_bytes,memory_);
                        const auto key=acknowledged_turn_key(event,fields);
                        if(!key)break;
                        AppServerRequests requests(memory_,1);requests.track(RpcSender::client,event);
                        (void)requests.bind(bytes,RpcSender::server);
                        remember_turn_input(state,*key,input);
                        break;
                    }
                }
            }
            state.indexed_turn_deliveries=it->first+1;
        }
        const auto found=state.turn_inputs.find(turn_key(thread,turn));
        if(found==state.turn_inputs.end())return std::nullopt;
        if(requested){
            for(const auto& input:found->second)if(input.original==*requested)return input;
            // A unique native turn owner remains known even if a producer's
            // claimed address is wrong; tool_observation rejects that claim.
        }
        return found->second.size()==1?std::optional<Context::Delivery>(found->second.front()):std::nullopt;
    }
    std::optional<ExperienceLocation> tool_input_reference(const AgentEvent& event){
        if(event.native_name()!="item/completed")return std::nullopt;
        try {
            const auto& item=event.fields().at("params").at("item");
            if(item.at("type").string()!="mcpToolCall"||item.at("status").string()!="completed")return std::nullopt;
            return record_address(item.at("result").at("structuredContent").at("swegcaObservation").at("inputOriginal"));
        }catch(const std::invalid_argument&){return std::nullopt;}
        catch(const std::out_of_range&){return std::nullopt;}
    }
    // Producers may report observations, never core verdicts. Accept only an
    // explicit address-bound payload from an actual completed MCP tool item.
    struct ToolObservation {
        kernel::EvidenceObservation value;
        std::optional<std::string_view> scope;
    };
    std::optional<ToolObservation> tool_observation(const AgentEvent& event,
        const ExperienceLocation& input,const DigestBytes& hypothesis,std::uint64_t observed){
        using namespace kernel;
        if(event.native_name()!="item/completed")return std::nullopt;
        try {
            const auto& item=event.fields().at("params").at("item");
            if(item.at("type").string()!="mcpToolCall"||item.at("status").string()!="completed")return std::nullopt;
            if(const auto* error=item.find("error");error&&error->kind!=Json::Kind::null)return std::nullopt;
            const auto& fields=item.at("result").at("structuredContent").at("swegcaObservation");
            if(record_address(fields.at("inputOriginal"))!=input)return std::nullopt;
            std::optional<std::string_view> scope;
            if(const auto* declared=fields.find("scope")){
                scope=declared->string();if(scope->empty())return std::nullopt;
            }
            const auto* proposed=fields.find("requirement");
            if(!requirement_scope_matches(proposed,scope,memory_))return std::nullopt;
            if(proposed){
                if(!scope)return std::nullopt;
                const auto anchor=requirement_anchor(*proposed);
                const auto stored=runtime_.session().read_original(input);
                const auto payload=evidence_payload(stored);
                if(payload.sender!=ExperienceSender::client||payload.media_type!="application/json")return std::nullopt;
                const std::string_view bytes(reinterpret_cast<const char*>(payload.content.data()),payload.content.size());
                const auto native=parse_json(bytes,memory_);
                if(!requirement_matches(anchor,native))return std::nullopt;
            }
            for(const auto key:{"hypothesis","context","source","producer","status","verdict"})
                if(fields.find(key))return std::nullopt;
            EvidenceObservation value;value.hypothesis=hypothesis;value.context=input.digest;value.observed_at=observed;
            value.source=value.producer=agent_session_identity("mcp-observation",item.at("server").string(),item.at("tool").string());
            const auto axis=integer(fields.at("axis"));if(axis>UINT32_MAX)return std::nullopt;
            value.axis=static_cast<std::uint32_t>(axis);value.producer_confidence=real(fields.at("confidence"));
            value.expires_at=integer(fields.at("expiresAt"));
            const auto& expiry=fields.at("hasExpiry");if(expiry.kind!=Json::Kind::boolean)return std::nullopt;
            value.has_expiry=expiry.scalar=="true";
            const auto outcome=fields.at("outcome").string();
            if(outcome=="support")value.outcome=EvidenceOutcome::support;
            else if(outcome=="refute")value.outcome=EvidenceOutcome::refute;
            else if(outcome!="insufficient")return std::nullopt;
            const auto* connection=runtime_.session().find(hypothesis);
            if(!connection||!observation_values_valid(connection->rules(),hypothesis,value))return std::nullopt;
            value.hypothesis={};value.context={}; // Runtime derives these from the sealed original.
            return ToolObservation{value,scope};
        }catch(const std::invalid_argument&){return std::nullopt;}
        catch(const std::out_of_range&){return std::nullopt;}
    }
    void select_context(const DigestBytes& identity){
        const auto found=contexts_.find(identity);
        if(found==contexts_.end())throw std::invalid_argument("session not attached");
        if(!found->second.native_session.empty()&&!found->second.native_ready)
            throw std::invalid_argument("native recovery incomplete; reopen required");
        runtime_.select_session(identity);selected_=&found->second;
    }
    void attach_context(const DigestBytes& identity,std::string_view name,bool resume,bool select,
        std::string_view native = {},bool app_server=false,bool ensure=false,bool connection_scope=false) {
        if(select&&runtime_.has_session())throw std::invalid_argument("session already selected");
        // Reserve transport state before acquiring a Runtime lease. Failure
        // leaves the prior selection and all existing receipts untouched.
        auto [it,inserted]=contexts_.try_emplace(identity,memory_,native,app_server,connection_scope);
        if(!inserted)throw std::invalid_argument("session already attached");
        try {
            if(ensure)runtime_.attach_available_session(identity,name);
            else if(resume)runtime_.attach_resumed_session(identity);
            else runtime_.attach_session(identity,name);
        } catch(...) {contexts_.erase(it);throw;}
        if(select){runtime_.select_session(identity);selected_=&it->second;}
    }
    void payload_result(std::string_view id,std::string_view prefix,std::span<const std::byte> bytes){
        (*output_)<<"{\"jsonrpc\":\"2.0\",\"id\":"<<id<<",\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"";
        write_json_string_content((*output_),prefix);write_json_hex((*output_),bytes);
        (*output_)<<"\\\"}\"}],\"structuredContent\":"<<prefix;
        write_json_hex((*output_),bytes);(*output_)<<"\"}}}\n"<<std::flush;
        if(!(*output_))throw std::runtime_error("MCP output disconnected");
    }
    void tool_result(std::string_view id,std::string_view body){
        // The verified result is already materialized. Emit its two required
        // MCP representations without allocating escaped/combined duplicates.
        (*output_)<<"{\"jsonrpc\":\"2.0\",\"id\":"<<id<<",\"result\":{\"content\":[{\"type\":\"text\",\"text\":";
        write_json_string((*output_),body);
        (*output_)<<"}],\"structuredContent\":"<<body<<"}}\n"<<std::flush;
        if(!(*output_))throw std::runtime_error("MCP output disconnected");
    }
    void result(std::string_view id,std::string_view body){(*output_)<<"{\"jsonrpc\":\"2.0\",\"id\":"<<id<<",\"result\":"<<body<<"}\n"<<std::flush;if(!(*output_))throw std::runtime_error("MCP output disconnected");}
    void error(std::string_view id,int code,std::string_view message){(*output_)<<"{\"jsonrpc\":\"2.0\",\"id\":"<<id<<",\"error\":{\"code\":"<<code<<",\"message\":"<<quote_json(message,memory_)<<"}}\n"<<std::flush;}
    void clear_replay(){
        context().replayed.reset();context().cognition.reset();context().cognition_done=false;
    }
    void clear(){clear_replay();context().cognition_connection.reset();context().cognition_saved=false;context().cognition_snapshot_saved=false;context().cognition_revision.reset();context().cognition_parameters.reset();context().received.reset();context().recovered_cognition.reset();context().recovered_receipt=0;}
    const ReplayedInput* selected_replay() const {
        if(context().replayed)return &*context().replayed;
        return context().cognition ? &context().cognition->replayed : nullptr;
    }
    void invalidate_cognition(const RecordedRefinement& recorded){
        auto& state=context();
        if(state.scoped&&state.scoped->cognition->assessment().remembered_head().identity==recorded.refinement.connection()){
            state.scoped->parameters={recorded.refinement.seed(),recorded.refinement.current_step()};
            state.scoped->dirty=true;state.scoped->saved=false;state.scoped->revision.reset();
        }
        if(state.restored&&state.restored->cognition->assessment().remembered_head().identity==recorded.refinement.connection()){
            state.restored->parameters={recorded.refinement.seed(),recorded.refinement.current_step()};
            state.restored->dirty=true;state.restored->saved=false;state.restored->revision.reset();
        }
        if(state.related&&state.related->cognition->assessment().remembered_head().identity==recorded.refinement.connection()){
            state.related->parameters={recorded.refinement.seed(),recorded.refinement.current_step()};
            state.related->dirty=true;state.related->saved=false;state.related->revision.reset();
        }
        if(state.cognition_connection&&*state.cognition_connection==recorded.refinement.connection()){
            state.cognition_parameters=std::pair{recorded.refinement.seed(),recorded.refinement.current_step()};
            // Preserve the immutable input-time receipt; refresh only the live comparison.
            state.cognition_done=false;
        }
    }
    void invalidate_main_cognition(){
        for(auto& [identity,state]:contexts_){
            const auto& session=runtime_.attached_session(identity);
            const auto changed=[&](const InputCognition& cognition){
                const auto connection=cognition.assessment().remembered_head().identity;
                return cognition.replayed.from_merged_main()&&!session.find(connection)&&
                    cognition.assessment().current_head().record!=runtime_.main().head();
            };
            const auto parameters=[&](const InputCognition& cognition,auto prior){
                const auto* report=runtime_.main().graph().refinement(cognition.assessment().remembered_head().identity);
                if(report&&report->current_step()>=prior.second)prior={report->seed(),report->current_step()};
                return prior;
            };
            const auto invalidate=[&](auto& saved){
                if(saved&&changed(*saved->cognition)){
                    saved->parameters=parameters(*saved->cognition,saved->parameters);
                    saved->dirty=true;saved->saved=false;saved->revision.reset();
                }
            };
            invalidate(state.scoped);invalidate(state.restored);invalidate(state.related);
            if(state.cognition&&state.received&&changed(*state.cognition)){
                const auto& recorded=state.received->recorded.refinement;
                state.cognition_parameters=parameters(*state.cognition,
                    state.cognition_parameters.value_or(std::pair{recorded.seed(),recorded.current_step()}));
                state.cognition_done=false;
            }
        }
    }
    void complete_scoped_cognition(){
        auto& scoped=context().scoped;
        if(!scoped)return;
        const auto [seed,step]=scoped->parameters;
        if(scoped->dirty){
            auto& remembered=*scoped->cognition;
            auto compared=runtime_.compare_replay(remembered.replayed,seed,step);
            std::optional<ReEvidenceResult> verified;
            if(kernel::requires_re_evidence(compared.agreement()))
                verified.emplace(runtime_.re_evidence(remembered.replayed,compared,seed,step));
            InputCognition refreshed{remembered.candidate,std::move(remembered.replayed),std::move(compared),std::move(verified)};
            scoped->cognition.reset();scoped->cognition.emplace(std::move(refreshed));scoped->dirty=false;
        }
        if(!scoped->saved){
            const auto& input=scoped->input;
            const auto& cognition=*scoped->cognition;
            auto metadata=std::pmr::string("{\"inputOriginal\":",&memory_)+address(input,memory_);
            metadata+=",\"scope\":";metadata+=quote_json(scoped->scope,memory_);
            metadata+=",\"scopeConnection\":\"";metadata+=hex(cognition.assessment().remembered_head().identity,memory_);
            metadata+="\",\"observationBoundary\":\"";metadata+=std::to_string(cognition.comparison.observation_boundary());
            metadata+="\",\"seed\":\"";metadata+=std::to_string(seed);metadata+="\",\"step\":\"";metadata+=std::to_string(step);
            metadata+="\",\"sourceSession\":\"";metadata+=hex(cognition.replayed.source_identity(),memory_);
            metadata+="\",\"selectedOriginal\":";metadata+=address(cognition.replayed.location(),memory_);
            metadata+=",\"originalIndex\":\"";metadata+=std::to_string(cognition.replayed.original_index());metadata+='"';
            metadata+=",\"observationHead\":";metadata+=address(cognition.replayed.observation_head(),memory_);
            metadata+=",\"replayPrefix\":";metadata+=quote_json(scoped_replay_prefix(*scoped),memory_);metadata+='}';
            scoped->revision=runtime_.save_cognition_revision(input,std::as_bytes(std::span(metadata)),
                input_observation_scope(input.digest,scoped->scope));
            scoped->saved=true;
        }
    }
    void complete_restored_cognition(){
        complete_saved_cognition(context().restored);
        complete_saved_cognition(context().related);
    }
    void complete_saved_cognition(std::optional<Context::RestoredCognition>& restored){
        if(!restored)return;
        const auto [seed,step]=restored->parameters;
        if(restored->dirty){
            auto& prior=*restored->cognition;
            auto compared=runtime_.compare_replay(prior.replayed,seed,step);
            std::optional<ReEvidenceResult> verified;
            if(kernel::requires_re_evidence(compared.agreement()))
                verified.emplace(runtime_.re_evidence(prior.replayed,compared,seed,step));
            InputCognition refreshed{prior.candidate,std::move(prior.replayed),std::move(compared),std::move(verified)};
            restored->cognition.reset();restored->cognition.emplace(std::move(refreshed));restored->dirty=false;
        }
        if(restored->saved)return;
        const auto& cognition=*restored->cognition;
        auto metadata=std::pmr::string("{\"inputOriginal\":",&memory_)+address(restored->input,memory_);
        metadata+=",\"seed\":\"";metadata+=std::to_string(seed);metadata+="\",\"step\":\"";metadata+=std::to_string(step);
        metadata+="\",\"memory\":{\"completed\":true,\"candidate\":\"";metadata+=std::to_string(cognition.candidate);
        metadata+="\",\"original\":";metadata+=address(cognition.replayed.location(),memory_);
        metadata+=",\"agreement\":";metadata+=std::to_string(static_cast<unsigned>(cognition.assessment().agreement()));
        metadata+=",\"reEvidencePerformed\":";metadata+=cognition.reverified?"true":"false";metadata+='}';
        metadata+=",\"sourceSession\":\"";metadata+=hex(cognition.replayed.source_identity(),memory_);
        metadata+="\",\"selectedOriginal\":";metadata+=address(cognition.replayed.location(),memory_);
        metadata+=",\"replayPrefix\":";metadata+=quote_json(replay_prefix(cognition.replayed,&cognition,&restored->input),memory_);
        metadata+=",\"recovery\":";metadata+=restored->recovery;
        if(restored->related_from){metadata+=",\"relatedFrom\":";metadata+=address(*restored->related_from,memory_);}
        if(restored->related_connection){metadata+=",\"relatedConnection\":\"";metadata+=hex(*restored->related_connection,memory_);metadata+='"';}
        metadata+='}';
        restored->revision=runtime_.save_cognition_revision(restored->input,std::as_bytes(std::span(metadata)),
            restored->related_from?std::optional{restored->related_connection?
                related_connection_cognition_channel(restored->input.digest,*restored->related_connection):
                related_cognition_channel(restored->input.digest)}:std::nullopt);
        restored->saved=true;
    }
    ReplayRecovery recovery_coordinates(const Json& metadata){
        const auto& value=metadata.at("recovery");ReplayRecovery result;
        auto flag=[&](std::string_view name){const auto& field=value.at(name);
            if(field.kind!=Json::Kind::boolean)throw std::invalid_argument("invalid recovery flag");
            return field.scalar=="true";};
        result.temporary=flag("temporary");result.seed_only=flag("seedOnly");
        const auto& kind=value.at("keyKind");
        if(kind.kind!=Json::Kind::number)throw std::invalid_argument("invalid recovery key kind");
        const auto k=number(kind.scalar);if(k<1||k>3)throw std::invalid_argument("invalid recovery key kind");
        result.key_kind=static_cast<kernel::FamiliarityKey>(k);
        result.input_cue=digest(value.at("inputCue").string());result.lookup_key=digest(value.at("lookupKey").string());
        result.connection=digest(value.at("connection").string());result.source=digest(metadata.at("sourceSession").string());
        result.original=record_address(metadata.at("selectedOriginal"));
        result.remembered_head=record_address(value.at("rememberedHead"));result.observation_head=record_address(value.at("observationHead"));
        const auto index=integer(value.at("originalIndex")),boundary=integer(value.at("observationBoundary"));
        if(index>SIZE_MAX||boundary>SIZE_MAX)throw std::invalid_argument("recovery index overflow");
        result.original_index=index;result.observation_boundary=boundary;return result;
    }
    std::pmr::string recovery_checkpoint(const InputRecall& recalled,const ReplayedInput& replayed,
        const ReplayComparison& compared){
        std::pmr::string metadata(&memory_);
        metadata+="{\"temporary\":";metadata+=recalled.temporary()?"true":"false";
        metadata+=",\"keyKind\":";metadata+=std::to_string(static_cast<unsigned>(recalled.key_kind()));
        metadata+=",\"lookupKey\":\"";metadata+=hex(recalled.lookup_key(),memory_);
        metadata+="\",\"inputCue\":\"";metadata+=hex(replayed.input_cue(),memory_);
        metadata+="\",\"seedOnly\":";metadata+=recalled.seed_only()?"true":"false";
        metadata+=",\"connection\":\"";metadata+=hex(compared.evidence().remembered_head().identity,memory_);
        metadata+="\",\"rememberedHead\":";metadata+=address(compared.evidence().remembered_head().record,memory_);
        metadata+=",\"observationHead\":";metadata+=address(replayed.observation_head(),memory_);
        metadata+=",\"originalIndex\":\"";metadata+=std::to_string(replayed.original_index());
        metadata+="\",\"observationBoundary\":\"";metadata+=std::to_string(compared.observation_boundary());
        metadata+="\"}";
        return metadata;
    }
    void append_recovery_checkpoint(std::pmr::string& metadata,const ReplayedInput& replayed,
        const ReplayComparison& compared){
        metadata+=",\"recovery\":";
        metadata+=recovery_checkpoint(context().received->recalled,replayed,compared);
    }
    void complete_cognition(){
        auto& state=context();
        if(!state.cognition_done){
            if(!state.received)throw std::logic_error("input receipt required before cognition");
            const auto& recorded=state.received->recorded.refinement;
            const auto parameters=state.cognition_parameters.value_or(std::pair{recorded.seed(),recorded.current_step()});
            std::optional<InputCognition> cognition;
            if(state.cognition){
                // The selected original is already authenticated and owned.
                // Refresh only its comparison against newly recorded evidence.
                auto compared=runtime_.compare_replay(state.cognition->replayed,parameters.first,parameters.second);
                std::optional<ReEvidenceResult> reverified;
                if(swegca::architecture::kernel::requires_re_evidence(compared.agreement()))
                    reverified.emplace(runtime_.re_evidence(state.cognition->replayed,compared,parameters.first,parameters.second));
                cognition.emplace(InputCognition{state.cognition->candidate,std::move(state.cognition->replayed),
                    std::move(compared),std::move(reverified)});
            }else{
                auto prepared=runtime_.cognize(state.received->recalled,parameters.first,parameters.second);
                if(prepared)cognition.emplace(std::move(*prepared));
            }
            state.replayed.reset();state.cognition.reset();
            if(cognition)state.cognition.emplace(std::move(*cognition));
            state.cognition_connection.reset();
            if(state.cognition)state.cognition_connection=state.cognition->assessment().remembered_head().identity;
            state.cognition_done=true;
            state.cognition_snapshot_saved=false;
        }
        if(!state.native_session.empty() && !state.cognition_snapshot_saved){
            auto metadata=std::pmr::string("{\"inputOriginal\":",&memory_)+address(state.received->recorded.original,memory_);
            const auto& recorded=state.received->recorded.refinement;
            const auto parameters=state.cognition_parameters.value_or(std::pair{recorded.seed(),recorded.current_step()});
            metadata+=",\"seed\":\"";metadata+=std::to_string(parameters.first);
            metadata+="\",\"step\":\"";metadata+=std::to_string(parameters.second);metadata+='"';
            cognition_body(metadata);
            if(state.cognition){
                metadata+=",\"sourceSession\":\"";metadata+=hex(state.cognition->replayed.source_identity(),memory_);
                metadata+="\",\"selectedOriginal\":";metadata+=address(state.cognition->replayed.location(),memory_);
                metadata+=",\"replayPrefix\":";metadata+=quote_json(replay_prefix(state.cognition->replayed,&*state.cognition),memory_);
                append_recovery_checkpoint(metadata,state.cognition->replayed,state.cognition->comparison);
            }else metadata+=",\"sourceSession\":null,\"selectedOriginal\":null,\"replayPrefix\":null";
            metadata+='}';
            if(!state.cognition_saved){
                runtime_.save_cognition(state.received->recorded.original,std::as_bytes(std::span(metadata)));
                state.cognition_saved=true;
            }else state.cognition_revision=runtime_.save_cognition_revision(
                state.received->recorded.original,std::as_bytes(std::span(metadata)));
            state.cognition_snapshot_saved=true;
        }
    }

    void cognition_body(std::pmr::string& body) const {
        body+=",\"memory\":{\"completed\":";body+=context().cognition_done?"true":"false";
        const auto& cognition=context().cognition;
        if(cognition){
            body+=",\"candidate\":\"";body+=std::to_string(cognition->candidate);body+="\",\"original\":";
            body+=address(cognition->replayed.location(),memory_);
            body+=",\"agreement\":";body+=std::to_string(static_cast<unsigned>(cognition->assessment().agreement()));
            body+=",\"reEvidencePerformed\":";body+=cognition->reverified?"true":"false";
        }else body+=",\"original\":null";
        body+='}';
    }
    static std::size_t candidate_limit(const Json& p) {
        const auto* value=p.find("candidateLimit");
        const auto limit=value?integer(*value):64;
        if(!limit||limit>256)throw std::invalid_argument("candidateLimit must be 1..256");
        return static_cast<std::size_t>(limit);
    }
    void candidate_page(std::pmr::string& body,std::uint64_t offset,std::size_t limit) const {
        const auto matches=context().received->recalled.matches();
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
    std::pmr::string host(std::string_view method,Json& p,std::string_view frame,const JsonMemberSource& source){
        if(method=="swegca/agent/attach"||method=="swegca/agent/attach/resume"||method=="swegca/agent/attach/ensure"){
            const auto provider=p.at("provider").string();
            if(provider!="codex")throw std::invalid_argument("native provider adapter unavailable");
            const auto native=p.at("session").string();
            const auto identity=agent_session_identity(provider,p.at("instance").string(),native);
            const auto* protocol=p.find("protocol");
            const auto format=protocol?protocol->string():"hook";
            if(format!="hook"&&format!="app-server"&&format!="app-server-connection")throw std::invalid_argument("unknown native protocol");
            const bool ensure=method=="swegca/agent/attach/ensure";
            if(!ensure||!contexts_.contains(identity))
                attach_context(identity,native,method=="swegca/agent/attach/resume",false,native,format!="hook",ensure,format=="app-server-connection");
            auto& state=contexts_.at(identity);
            if(state.native_session!=native||state.app_server!=(format!="hook")||state.connection_scope!=(format=="app-server-connection"))throw std::invalid_argument("native session binding mismatch");
            const auto& attached=runtime_.attached_session(identity);
            if(ensure&&(!attached.usable()||kernel::route_agent_event(attached.phase(),kernel::AgentEventKind::lifecycle)!=kernel::AgentEventRoute::record))
                throw std::invalid_argument("stored session no longer accepts lifecycle events");
            if(!state.native_ready)attached.visit_deliveries(state.native_session,state.native_source(),"application/json",&state,
                [](void* opaque,const OriginalDelivery& delivery){
                auto& target=*static_cast<Context*>(opaque);
                const auto [at,inserted]=target.deliveries.try_emplace(delivery.sequence(),
                    Context::Delivery{delivery.fingerprint(),delivery.original(),delivery.context(),delivery.sender(),delivery.connection()});
                if(!inserted && (at->second.fingerprint!=delivery.fingerprint() || at->second.original!=delivery.original()))
                    throw std::invalid_argument("conflicting stored native sequence");
            });
            if(!state.deliveries.empty() && (state.deliveries.begin()->first!=0 ||
               state.deliveries.rbegin()->first!=state.deliveries.size()-1))
                throw std::invalid_argument("noncontiguous stored native sequence");
            state.native_ready=true;
            return "{\"identity\":\""+hex(identity,memory_)+"\",\"nextSequence\":\""+std::to_string(state.deliveries.size()).c_str()+"\"}";
        }
        if(method=="swegca/agent/cognition"){
            const auto identity=digest(p.at("identity").string());
            const auto context_at=contexts_.find(identity);
            const auto* state=context_at==contexts_.end()?nullptr:&context_at->second;
            ExperienceLocation input;
            if(const auto* original=p.find("inputOriginal")){
                if(p.find("sequence"))throw std::invalid_argument("choose sequence or inputOriginal");
                input=record_address(*original);
            }else{
                if(!state||state->native_session.empty()||!state->native_ready)
                    throw std::invalid_argument("native session binding required");
                const auto found=state->deliveries.find(integer(p.at("sequence")));
                if(found==state->deliveries.end())throw std::invalid_argument("native delivery not recorded");
                input=found->second.original;
            }
            const auto* requested=p.find("revision");
            auto revision=requested?std::optional<DigestBytes>(digest(requested->string())):std::nullopt;
            const auto* newest=p.find("latest");
            if(newest&&newest->kind!=Json::Kind::boolean)throw std::invalid_argument("latest must be boolean");
            const bool latest=newest&&newest->scalar=="true";
            std::optional<std::string_view> scope;
            std::optional<DigestBytes> channel;
            if(const auto* value=p.find("scope")){
                scope=value->string();
                if(scope->empty()||(!latest&&!revision))throw std::invalid_argument("scope requires a name and latest or revision");
                channel=input_observation_scope(input.digest,*scope);
            }
            bool related=false;
            if(const auto* flag=p.find("related")){
                if(flag->kind!=Json::Kind::boolean)throw std::invalid_argument("related must be boolean");
                related=flag->scalar=="true";
            }
            std::optional<DigestBytes> related_connection;
            if(const auto* field=p.find("connection")){
                if(!related)throw std::invalid_argument("connection requires related cognition");
                related_connection=digest(field->string());
            }
            if(related){
                if(scope||(!latest&&!revision))throw std::invalid_argument("related requires latest or revision and no scope");
                channel=related_connection?related_connection_cognition_channel(input.digest,*related_connection):related_cognition_channel(input.digest);
            }
            const auto stored=runtime_.read_cognition_record(identity,input,revision,latest,channel);
            if(!stored)throw std::invalid_argument("cognition record not found");
            if(latest)revision=Sha256::of(stored->view().content);
            constexpr std::string_view input_path[]{"inputOriginal"};
            const auto input_metadata=parse_json_selected(content_text(*stored),memory_,input_path);
            if(record_address(input_metadata)!=input)
                throw std::runtime_error("cognition input binding mismatch");
            // The preceding selected parse validated the entire JSON. The
            // only optional-member failure here is the absent scope field.
            std::optional<Json> recorded_scope;
            constexpr std::string_view scope_path[]{"scope"};
            try{recorded_scope.emplace(parse_json_selected(content_text(*stored),memory_,scope_path));}
            catch(const std::invalid_argument&){ }
            if(scope?(!recorded_scope||recorded_scope->string()!=*scope):bool(recorded_scope))
                throw std::invalid_argument("cognition scope binding mismatch");
            std::optional<Json> related_origin;
            constexpr std::string_view related_path[]{"relatedFrom"};
            try{related_origin.emplace(parse_json_selected(content_text(*stored),memory_,related_path));}
            catch(const std::invalid_argument&){ }
            if(related!=bool(related_origin))throw std::invalid_argument("cognition relation binding mismatch");
            if(related_origin)(void)record_address(*related_origin);
            std::optional<Json> recorded_connection;
            constexpr std::string_view connection_path[]{"relatedConnection"};
            try{recorded_connection.emplace(parse_json_selected(content_text(*stored),memory_,connection_path));}
            catch(const std::invalid_argument&){ }
            if(related_connection?(!recorded_connection||digest(recorded_connection->string())!=*related_connection):bool(recorded_connection))
                throw std::invalid_argument("cognition connection binding mismatch");
            const bool related_live=related&&state&&state->related&&state->related->input==input&&
                state->related->related_connection==related_connection&&
                !state->related->dirty&&state->related->saved&&state->related->revision;
            const bool live=!related&&!scope&&state&&state->received&&state->received->recorded.original==input&&
                state->cognition_done&&state->cognition_snapshot_saved&&state->cognition_revision;
            const bool restored_live=!related&&!scope&&state&&state->restored&&state->restored->input==input&&
                !state->restored->dirty&&state->restored->saved&&state->restored->revision;
            const bool scoped_live=scope&&state&&state->scoped&&state->scoped->input==input&&state->scoped->scope==*scope&&!state->scoped->dirty&&state->scoped->saved&&state->scoped->revision;
            auto body=std::pmr::string("{\"revision\":",&memory_)+(revision?quote_json(hex(*revision,memory_),memory_):"null")+
                ",\"liveRevision\":"+(related_live?quote_json(hex(*state->related->revision,memory_),memory_):
                    scoped_live?quote_json(hex(*state->scoped->revision,memory_),memory_):
                    restored_live?quote_json(hex(*state->restored->revision,memory_),memory_):
                    live?quote_json(hex(*state->cognition_revision,memory_),memory_):"null")+
                ",\"record\":";
            const auto raw=content_text(*stored);
            if(body.size()==body.max_size()||raw.size()>body.max_size()-body.size()-1)
                throw std::length_error("cognition response size overflow");
            body.reserve(body.size()+raw.size()+1);body.append(raw);body+='}';return body;
        }
        if(method=="swegca/agent/original"){
            select_context(digest(p.at("identity").string()));
            const auto& state=context();
            if(state.native_session.empty()||!state.native_ready)
                throw std::invalid_argument("native session binding required");
            const auto found=state.deliveries.find(integer(p.at("sequence")));
            if(found==state.deliveries.end())throw std::invalid_argument("native delivery not recorded");
            const auto stored=runtime_.session().read_original(found->second.original);
            const auto original=evidence_payload(stored);
            if(original.source!=state.native_source()||original.session!=state.native_session||
               original.media_type!="application/json"||original.sequence!=found->first||original.sender!=found->second.sender)
                throw std::invalid_argument("native original binding mismatch");
            const std::string_view bytes(reinterpret_cast<const char*>(original.content.data()),original.content.size());
            if(agent_delivery_identity(original.sequence,original.observed_at_ns,bytes)!=found->second.fingerprint)
                throw std::invalid_argument("native original fingerprint mismatch");
            return "{\"original\":"+address(found->second.original,memory_)+
                ",\"context\":\""+hex(found->second.context,memory_)+"\",\"source\":"+
                quote_json(original.source,memory_)+",\"sender\":"+
                (original.sender==ExperienceSender::unspecified?"null":original.sender==ExperienceSender::client?"\"client\"":"\"server\"")+",\"observedAt\":\""+
                std::to_string(original.observed_at_ns).c_str()+"\",\"native\":"+quote_json(bytes,memory_)+"}";
        }
        if(method=="swegca/agent/event"){
            // A targeted native request selects its already attached in-memory
            // route within this call; no separate select RPC or storage discovery.
            if(const auto* identity=p.find("identity"))select_context(digest(identity->string()));
            auto& state=context();
            if(state.native_session.empty()||!state.native_ready)throw std::invalid_argument("native session binding required");
            std::optional<AgentEvent> parsed_event;
            std::optional<AppServerRequests> binding;
            std::optional<AppServerRequests::Response> response;
            std::optional<ExperienceLocation> request_original;
            DigestBytes request_connection{};
            std::optional<std::pmr::string> committed_turn_key;
            std::optional<Context::Delivery> committed_turn_input;
            ExperienceSender sender=ExperienceSender::unspecified;
            if(const auto* incoming=p.find("sender")){
                if(!state.app_server)throw std::invalid_argument("sender requires app-server binding");
                if(incoming->string()=="client")sender=ExperienceSender::client;
                else if(incoming->string()=="server")sender=ExperienceSender::server;
                else throw std::invalid_argument("invalid native sender");
            }
            if(const auto* request_sequence=p.find("requestSequence")){
                if(!state.app_server)throw std::invalid_argument("response requires app-server binding");
                const auto request=state.deliveries.find(integer(*request_sequence));
                if(request==state.deliveries.end()||request->first>=integer(p.at("sequence")))
                    throw std::invalid_argument("response requires earlier committed request");
                auto stored=runtime_.session().read_original(request->second.original);
                const auto original=evidence_payload(stored);
                if(original.source!=state.native_source()||original.session!=state.native_session||original.media_type!="application/json")
                    throw std::invalid_argument("request original binding mismatch");
                const std::string_view bytes(reinterpret_cast<const char*>(original.content.data()),original.content.size());
                if((sender==ExperienceSender::unspecified)!=(original.sender==ExperienceSender::unspecified)||
                   (sender!=ExperienceSender::unspecified&&original.sender==sender))
                    throw std::invalid_argument("response sender does not oppose recorded request");
                auto request_event=state.connection_scope?adapt_codex_app_server_connection(bytes,state.native_session,memory_):adapt_codex_app_server(bytes,memory_);
                if(request_event.session()!=state.native_session)throw std::invalid_argument("request session mismatch");
                binding.emplace(memory_,1);binding->track(RpcSender::client,request_event);
                response.emplace(binding->bind(p.at("native").kind==Json::Kind::object?source.bytes(frame):p.at("native").string(),RpcSender::server));
                request_original=stored.location();
                // Bind to the connection admitted with the sealed original,
                // not a newly reconstructed key from its transport envelope.
                request_connection=request->second.connection;
                if(sender==ExperienceSender::server&&original.sender==ExperienceSender::client){
                    committed_turn_key=acknowledged_turn_key(request_event,response->event().fields());
                    if(committed_turn_key)committed_turn_input=request->second;
                }
            }else{
                auto& native=p.at("native");
                if(native.kind==Json::Kind::object){
                    const auto bytes=source.bytes(frame);
                    if(state.app_server)parsed_event.emplace(adapt_parsed_codex_app_server(bytes,std::move(native),memory_,
                        state.connection_scope?std::string_view(state.native_session):std::string_view{}));
                    else parsed_event.emplace(adapt_codex_hook(bytes,memory_));
                }else{
                    (void)native.string();
                    if(state.app_server)
                        parsed_event.emplace(adapt_owned_codex_app_server(std::move(native.scalar),memory_,
                            state.connection_scope?std::string_view(state.native_session):std::string_view{}));
                    else parsed_event.emplace(adapt_codex_hook(native.string(),memory_));
                }
            }
            const auto& event=response?response->event():*parsed_event;
            SWEGCA_INGRESS_STAGE("host_native_adapted");
            if(event.session()!=state.native_session)throw std::invalid_argument("native session mismatch");
            if(sender==ExperienceSender::server&&event.kind()==swegca::architecture::kernel::AgentEventKind::input)
                throw std::invalid_argument("input must originate from client");
            if(sender==ExperienceSender::server&&event.native_name()=="thread/resume")
                throw std::invalid_argument("thread resume must originate from client");
            if(sender==ExperienceSender::client&&event.native_name()=="thread/started")
                throw std::invalid_argument("thread started must originate from server");
            // A notification's turn ID is a provenance reference, not a
            // successful outcome. Keep the observation insufficient until an
            // actual producer supplies evidence about the recorded input.
            if(!request_original&&sender==ExperienceSender::server&&state.app_server&&
                !state.deliveries.contains(integer(p.at("sequence")))&&!event.fields().find("id")&&
                event.kind()==swegca::architecture::kernel::AgentEventKind::content){
                const auto* params=event.fields().find("params");
                const auto* turn=params&&params->kind==Json::Kind::object?params->find("turnId"):nullptr;
                const auto* thread=params&&params->kind==Json::Kind::object?params->find("threadId"):nullptr;
                if(turn&&turn->kind==Json::Kind::string&&thread&&thread->kind==Json::Kind::string){
                    if(const auto input=turn_input(state,thread->scalar,turn->scalar,tool_input_reference(event))){
                        request_original=input->original;request_connection=input->connection;
                    }
                }
            }
            const auto sequence=integer(p.at("sequence")),observed=integer(p.at("observedAt"));
            const auto seed=integer(p.at("seed")),step=integer(p.at("step"));
            const OriginalExperienceView original{sequence,observed,state.native_session,state.native_source(),
                "application/json",std::as_bytes(std::span(event.native_bytes())),sender};
            using namespace swegca::architecture::kernel;
            const auto route=route_agent_event(runtime_.session().phase(),event.kind());
            if(route!=AgentEventRoute::recall_then_record && route!=AgentEventRoute::record)
                throw std::invalid_argument("native event route unavailable");
            const auto limit=route==AgentEventRoute::recall_then_record?candidate_limit(p):64;
            const auto found=state.deliveries.find(sequence);
            // Only retransmissions hash their full envelope before rejection or
            // acknowledgement. New input reaches Recall before this extra hash.
            const auto fingerprint=found==state.deliveries.end()?DigestBytes{}:
                agent_delivery_identity(sequence,observed,event.native_bytes());
            const auto delivery=route_agent_delivery(runtime_.session().phase(),found!=state.deliveries.end(),
                found!=state.deliveries.end() && found->second.fingerprint==fingerprint && found->second.sender==sender &&
                    (!request_original||found->second.context==request_original->digest),
                !state.deliveries.empty(),state.deliveries.empty()?0:state.deliveries.rbegin()->first,sequence);
            if(delivery==AgentDeliveryRoute::reject)throw std::invalid_argument("conflicting or out-of-order native delivery");
            if(delivery==AgentDeliveryRoute::reuse){
                complete_scoped_cognition();complete_restored_cognition();
                std::pmr::string body("{\"duplicate\":true,\"original\":",&memory_);
                body+=address(found->second.original,memory_);body+=",\"receipt\":";
                if(route==AgentEventRoute::record&&state.received&&state.cognition_connection&&
                    *state.cognition_connection==found->second.connection&&
                    (!state.cognition_done||!state.cognition_snapshot_saved))complete_cognition();
                if(state.received && state.received->recorded.original==found->second.original){
                    complete_cognition();
                    body+="\""+std::to_string(state.receipt)+"\"";
                    cognition_body(body);
                }else if(route==AgentEventRoute::recall_then_record){
                    auto saved=runtime_.session().read_cognition(found->second.original);
                    if(saved){
                        if(next_receipt_==UINT64_MAX)throw std::overflow_error("receipt sequence exhausted");
                        const auto metadata=parse_json(content_text(*saved),memory_);
                        if(record_address(metadata.at("inputOriginal"))!=found->second.original)
                            throw std::runtime_error("saved cognition input mismatch");
                        const auto ticket=next_receipt_+1;
                        body+='"';body+=std::to_string(ticket);body+="\",\"memory\":";
                        body+=encode_json(metadata.at("memory"),memory_);
                        state.recovered_cognition.emplace(std::move(*saved));
                        state.recovered_receipt=ticket;next_receipt_=ticket;
                    }else body+="null,\"memory\":{\"completed\":false,\"original\":null}";
                }else body+="null";
                body+='}';return body;
            }
            // Reserve the index node before mutating durable experience state.
            const auto slot=state.deliveries.try_emplace(sequence,Context::Delivery{fingerprint,{}, {},sender}).first;
            bool committed=false;
            try {
                if(route==AgentEventRoute::recall_then_record){
                    if(next_receipt_==std::numeric_limits<std::uint64_t>::max())throw std::overflow_error("receipt sequence exhausted");
                    const auto prompt=event.cue_content();
                    SWEGCA_INGRESS_STAGE("host_cue_ready");
                    clear();state.receipt=++next_receipt_;
                    try{state.received.emplace(runtime_.receive_envelope(event.cue_media(),std::as_bytes(std::span(prompt)),original,seed,step));}
                    catch(...){state.scoped.reset();state.restored.reset();state.related.reset();throw;}
                    // Release the prior scoped Replay only after this input's
                    // Deja vu/Recall; no new cache destruction precedes them.
                    state.scoped.reset();state.restored.reset();state.related.reset();
                    slot->second.original=state.received->recorded.original;
                    slot->second.context=state.received->recorded.context;
                    slot->second.connection=state.received->recorded.refinement.connection();committed=true;
                    slot->second.fingerprint=agent_delivery_identity(sequence,observed,event.native_bytes());
                    if(state.indexed_turn_deliveries==sequence)++state.indexed_turn_deliveries;
                    complete_cognition();
                    return received_body(limit);
                }
                bool admitted_tool_observation=false;
                auto recorded=[&]{
                    if(!request_original)return runtime_.retain(original,seed,step);
                    if(!response&&sender==ExperienceSender::server){
                        if(const auto observed_value=tool_observation(event,*request_original,request_connection,observed)){
                            auto result=observed_value->scope?
                                runtime_.observe_input_scope(*request_original,*observed_value->scope,original,observed_value->value,seed,step):
                                runtime_.observe_input(*request_original,original,observed_value->value,seed,step);
                            admitted_tool_observation=true;return result;
                        }
                    }
                    EvidenceObservation observation;observation.hypothesis=request_connection;
                    observation.context=request_original->digest;observation.observed_at=observed;
                    Sha256 source;source.update("SWEGCA input source v1");source.update(original.source);
                    observation.source=observation.producer=source.finish();
                    return runtime_.observe(request_connection,original,observation,seed,step);
                }();
                slot->second.original=recorded.original;
                slot->second.context=recorded.context;
                slot->second.connection=recorded.refinement.connection();committed=true;
                invalidate_cognition(recorded);
                if(binding)binding->recorded(*response);
                slot->second.fingerprint=agent_delivery_identity(sequence,observed,event.native_bytes());
                // Publish only after the real response has been recorded. A
                // failed publication leaves the recovery cursor behind it.
                if(committed_turn_key)remember_turn_input(state,*committed_turn_key,*committed_turn_input);
                if(state.indexed_turn_deliveries==sequence)++state.indexed_turn_deliveries;
                // Finish live comparison and durable revision before acknowledging
                // the observation. Only the core's conflict opens Re-evidence.
                complete_restored_cognition();
                if(admitted_tool_observation){
                    complete_scoped_cognition();
                    if(state.received&&!state.cognition_done)complete_cognition();
                }
                return "{\"original\":"+address(recorded.original,memory_)+",\"refinement\":"+refinement(recorded.refinement,memory_)+"}";
            } catch(...) {
                if(!committed)state.deliveries.erase(slot);
                state.cognition_done=false;
                throw;
            }

        }
        if(method=="swegca/candidates"){
            if(!context().received||integer(p.at("receipt"))!=context().receipt)throw std::invalid_argument("expired receipt");
            const auto limit=candidate_limit(p);
            std::pmr::string body("{\"receipt\":\"",&memory_);body+=std::to_string(context().receipt);body+="\",";
            candidate_page(body,integer(p.at("offset")),limit);body+='}';return body;
        }
        if(method=="swegca/start"||method=="swegca/resume"||method=="swegca/attach"||method=="swegca/attach/resume"){
            const bool resume=method=="swegca/resume"||method=="swegca/attach/resume";
            attach_context(digest(p.at("identity").string()),resume?std::string_view{}:p.at("name").string(),resume,
                method=="swegca/start"||method=="swegca/resume");
            return std::pmr::string("{}",&memory_);
        }
        if(method=="swegca/select"){
            select_context(digest(p.at("identity").string()));
            return std::pmr::string("{}",&memory_);
        }
        if(method=="swegca/end"){
            auto target=contexts_.end();
            if(const auto* identity=p.find("identity"))target=contexts_.find(digest(identity->string()));
            else {
                (void)context();
                for(auto it=contexts_.begin();it!=contexts_.end();++it)
                    if(&it->second==selected_){target=it;break;}
            }
            if(target==contexts_.end())throw std::invalid_argument("end target session not attached");
            // End the named owner, never whichever session last received input.
            // Do not invalidate another session's Recall/Replay on this path.
            runtime_.end_session(target->first);
            if(&target->second==selected_)selected_=nullptr;
            contexts_.erase(target);
            if(automatic_)work_requested_=true;
            return std::pmr::string("{}",&memory_);
        }
        if(method=="swegca/work"){
            const auto before=runtime_.main().head();
            const auto count=runtime_.work(integer(p.at("seed")),integer(p.at("step")));
            if(before!=runtime_.main().head())invalidate_main_cognition();
            return std::pmr::string("{\"merged\":\"",&memory_)+std::to_string(count).c_str()+"\"}";
        }
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
            if(!context().native_session.empty()&&!(method=="swegca/observe"&&p.find("inputOriginal")))
                throw std::invalid_argument("native session requires agent event ingress");
            const auto page_limit=method=="swegca/receive"?candidate_limit(p):64;
            const auto sequence=integer(p.at("sequence")),observed=integer(p.at("observedAt")),seed=integer(p.at("seed")),step=integer(p.at("step"));
            const auto session=p.at("session").string(),source=p.at("source").string(),media=p.at("media").string();
            if(!context().native_session.empty()&&(!context().native_ready||
                session!=context().native_session||source==context().native_source()))
                throw std::invalid_argument("bound observation requires its own producer source and native session");
            std::pmr::vector<std::byte> binary(&memory_);std::span<const std::byte> content;
            const auto* text=p.find("content");const auto* encoded=p.find("contentHex");
            if(bool(text)==bool(encoded))throw std::invalid_argument("exactly one content encoding is required");
            if(text){const auto value=text->string();content=std::as_bytes(std::span(value));}
            else {const auto value=encoded->string();if(value.size()%2)throw std::invalid_argument("invalid content hex");
                for(std::size_t i=0;i<value.size();i+=2){unsigned byte=0;auto r=std::from_chars(value.data()+i,value.data()+i+2,byte,16);if(r.ec!=std::errc{}||r.ptr!=value.data()+i+2)throw std::invalid_argument("invalid content hex");binary.push_back(std::byte(byte));}content=binary;}
            if(method=="swegca/retain"){
                // Session events use the same SWEGCA admission, shuffle and
                // refinement path without replacing the current input/Replay.
                try {
                    auto recorded=runtime_.retain({sequence,observed,session,source,media,content},seed,step);
                    invalidate_cognition(recorded);complete_restored_cognition();
                    return "{\"original\":"+address(recorded.original,memory_)+",\"refinement\":"+refinement(recorded.refinement,memory_)+"}";
                }catch(...){
                    // A partial record can make the session unusable. Force
                    // validation before any old automatic result is exported.
                    context().cognition_done=false;context().cognition_saved=false;throw;
                }
            }
            if(method=="swegca/observe"){
                using namespace swegca::architecture::kernel;
                const auto& fields=p.at("observation");EvidenceObservation value;
                if(p.find("scope")||fields.find("scope"))
                    throw std::invalid_argument("scoped observation requires native tool result ingress");
                const auto* input_original=p.find("inputOriginal");
                if(input_original){
                    if(fields.find("hypothesis")||fields.find("context"))
                        throw std::invalid_argument("inputOriginal supplies hypothesis and context");
                    if(!context().native_session.empty()){
                        const auto stored=runtime_.session().read_original(record_address(*input_original));
                        const auto input=evidence_payload(stored);
                        if(input.source!=context().native_source()||input.session!=context().native_session||
                            input.media_type!="application/json")
                            throw std::invalid_argument("observation target must be a native original");
                    }
                }else{
                    value.hypothesis=digest(fields.at("hypothesis").string());
                    value.context=digest(fields.at("context").string());
                }
                value.source=digest(fields.at("source").string());value.producer=digest(fields.at("producer").string());
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
                auto& state=context();
                const bool affects_cognition=state.cognition_connection&&
                    *state.cognition_connection==value.hypothesis;
                // A failed write may poison the session after a durable prefix.
                // Never allow the cached assessment to bypass that failure.
                if(affects_cognition||input_original){state.cognition_done=false;state.cognition_saved=false;}
                const OriginalExperienceView original{sequence,observed,session,source,media,content};
                auto recorded=input_original?
                    runtime_.observe_input(record_address(*input_original),original,value,seed,step):
                    runtime_.observe(value.hypothesis,original,value,seed,step);
                invalidate_cognition(recorded);complete_restored_cognition();
                return "{\"original\":"+address(recorded.original,memory_)+",\"refinement\":"+refinement(recorded.refinement,memory_)+"}";
            }
            if(next_receipt_==std::numeric_limits<std::uint64_t>::max())throw std::overflow_error("receipt sequence exhausted");
            clear();context().receipt=++next_receipt_;
            try{context().received.emplace(runtime_.receive({sequence,observed,session,source,media,content},seed,step));}
            catch(...){context().scoped.reset();context().restored.reset();context().related.reset();throw;}
            context().scoped.reset();context().restored.reset();context().related.reset();
            complete_cognition();
            return received_body(page_limit);
        }
        throw std::invalid_argument("unknown SWEGCA host method");
    }
    std::pmr::string received_body(std::size_t limit){
            std::pmr::string body("{\"receipt\":\"",&memory_);body+=std::to_string(context().receipt);body+="\",\"original\":";body+=address(context().received->recorded.original,memory_);
            body+=",\"temporary\":";body+=context().received->recalled.temporary()?"true":"false";body+=',';
            candidate_page(body,0,limit);
            body+=",\"refinement\":";body+=refinement(context().received->recorded.refinement,memory_);
            cognition_body(body);body+='}';return body;
    }
    std::pmr::string replay_prefix(const ReplayedInput& replayed,const InputCognition* cognition=nullptr,const ExperienceLocation* input=nullptr){
        const auto value=evidence_payload(replayed.original());
        auto prefix="{\"original\":"+address(replayed.location(),memory_)+
            ",\"media\":"+quote_json(value.media_type,memory_)+",\"source\":"+quote_json(value.source,memory_)+
            ",\"session\":"+quote_json(value.session,memory_)+",\"observedAt\":\""+
            std::to_string(value.observed_at_ns).c_str()+"\",\"grantsAuthority\":false,\"assessment\":";
        if(cognition){
            const auto& assessment=cognition->assessment();
            const auto& judgment=assessment.verification().result().verification().judgment();
            prefix+="{\"agreement\":";prefix+=std::to_string(static_cast<unsigned>(assessment.agreement()));
            prefix+=",\"status\":";prefix+=std::to_string(static_cast<unsigned>(judgment.status()));
            prefix+=",\"reason\":";prefix+=std::to_string(static_cast<unsigned>(judgment.reason()));
            prefix+=",\"step\":\"";prefix+=std::to_string(assessment.verification().current_step());
            prefix+="\",\"inputOriginal\":";prefix+=address(input?*input:context().received->recorded.original,memory_);
            prefix+=",\"rememberedHead\":";prefix+=address(assessment.remembered_head().record,memory_);
            prefix+=",\"currentHead\":";prefix+=address(assessment.current_head().record,memory_);
            prefix+=",\"currentOriginalCount\":\"";prefix+=std::to_string(assessment.current_originals().size());
            prefix+="\",\"reEvidencePerformed\":";prefix+=cognition->reverified?"true":"false";prefix+='}';
        }else prefix+="null";
        prefix+=",\"contentHex\":\"";
        return prefix;
    }
    void replay_payload(std::string_view id,const ReplayedInput& replayed,const InputCognition* cognition=nullptr){
        auto prefix=replay_prefix(replayed,cognition);
        if(cognition&&!context().native_session.empty())related_availability(prefix,replayed);
        payload_result(id,prefix,evidence_payload(replayed.original()).content);
    }
    void related_availability(std::pmr::string& prefix,const ReplayedInput& parent){
        const auto references=runtime_.related(parent);
        constexpr std::string_view content_field=",\"contentHex\":\"";
        prefix.resize(prefix.size()-content_field.size());
        prefix+=",\"relatedAvailable\":";prefix+=references.familiar()?"true":"false";
        prefix+=content_field;
    }
    std::pmr::string scoped_replay_prefix(const Context::ScopedCognition& scoped){
        const auto& cognition=*scoped.cognition;
        auto prefix=replay_prefix(cognition.replayed,&cognition,&scoped.input);
        constexpr std::string_view content_field=",\"contentHex\":\"";
        prefix.resize(prefix.size()-content_field.size());
        prefix+=",\"scope\":";prefix+=quote_json(scoped.scope,memory_);
        prefix+=",\"temporary\":";prefix+=scoped.temporary?"true":"false";
        prefix+=",\"parentCognitionUnchanged\":true";prefix+=content_field;return prefix;
    }
    std::pair<std::uint64_t,std::uint64_t> current_parameters(const DigestBytes& connection,
        std::pair<std::uint64_t,std::uint64_t> parameters){
        if(const auto* local=runtime_.session().find(connection)){
            const auto latest=local->latest_refinement_parameters();
            if(latest.second>=parameters.second)parameters=latest;
        }else if(const auto* latest=runtime_.main().graph().refinement(connection)){
            if(latest->current_step()>=parameters.second)parameters={latest->seed(),latest->current_step()};
        }
        return parameters;
    }
    Context::RestoredCognition restore_saved_cognition(const ExperienceLocation& input,const Json& metadata){
        if(record_address(metadata.at("inputOriginal"))!=input)throw std::invalid_argument("saved cognition input mismatch");
        const auto checkpoint=recovery_coordinates(metadata);
        auto parameters=std::pair{integer(metadata.at("seed")),integer(metadata.at("step"))};
        parameters=current_parameters(checkpoint.connection,parameters);
        auto cognition=runtime_.restore_cognition(input,checkpoint,parameters.first,parameters.second);
        cognition.candidate=integer(metadata.at("memory").at("candidate"));
        Context::RestoredCognition prepared(input,std::move(cognition),parameters,encode_json(metadata.at("recovery"),memory_),memory_);
        return prepared;
    }
    void related_connections_result(std::string_view id,const Json& request,const ExperienceLocation& input,
        const ReplayedInput& parent){
        if(context().native_session.empty())throw std::invalid_argument("connection listing requires native input");
        if(request.kind!=Json::Kind::object)throw std::invalid_argument("connections requires an object");
        for(const auto& key:request.keys)if(key!="limit"&&key!="after"&&key!="snapshot")
            throw std::invalid_argument("unsupported connection page argument");
        const auto limit=integer(request.at("limit"));
        if(!limit||limit>64)throw std::invalid_argument("connection page limit must be 1..64");
        std::optional<DigestBytes> after;
        if(const auto* field=request.find("after"))after=digest(field->string());
        if(after&&!request.find("snapshot"))throw std::invalid_argument("continuation requires snapshot");
        auto recalled=runtime_.related(parent);
        auto page=runtime_.select_replay_connections(recalled,limit,after?&*after:nullptr);
        if(const auto* expected=request.find("snapshot");expected&&digest(expected->string())!=page.snapshot)
            throw std::invalid_argument("related connections changed; restart listing");
        std::pmr::string body("{\"relatedFrom\":",&memory_);body+=address(parent.location(),memory_);
        body+=",\"inputOriginal\":";body+=address(input,memory_);
        body+=",\"grantsAuthority\":false,\"requirementsComplete\":false,\"selectionOnly\":true,\"snapshot\":\"";
        body+=hex(page.snapshot,memory_);body+="\",\"temporary\":";body+=recalled.temporary()?"true":"false";
        body+=",\"connections\":[";bool first=true;
        for(const auto& entry:page.entries){
            if(!first)body+=',';first=false;
            body+="{\"connection\":\"";body+=hex(entry.connection,memory_);
            body+="\",\"original\":";body+=address(recalled.matches()[entry.candidate].original,memory_);body+='}';
        }
        body+="],\"next\":";
        if(page.next){body+='"';body+=hex(*page.next,memory_);body+='"';}else body+="null";
        body+='}';tool_result(id,body);
    }
    void related_replay_result(std::string_view id,const ExperienceLocation& input,
        const ReplayedInput& parent,std::pair<std::uint64_t,std::uint64_t> parameters,
        const std::optional<DigestBytes>& connection={}){
        if(context().native_session.empty())throw std::invalid_argument("related Replay requires a native input journal");
        auto& state=context();
        if(!state.related||state.related->input!=input||state.related->related_connection!=connection){
            complete_saved_cognition(state.related);
            const auto channel=connection?related_connection_cognition_channel(input.digest,*connection):related_cognition_channel(input.digest);
            const auto saved=runtime_.session().read_latest_cognition(input,channel);
            if(saved){
                const auto metadata=parse_json(content_text(*saved),memory_);
                if(record_address(metadata.at("relatedFrom"))!=parent.location())
                    throw std::invalid_argument("related parent original mismatch");
                const auto coordinates=recovery_coordinates(metadata);
                const auto* recorded_connection=metadata.find("relatedConnection");
                if(connection?(!recorded_connection||digest(recorded_connection->string())!=*connection||coordinates.connection!=*connection):bool(recorded_connection))
                    throw std::invalid_argument("related connection journal mismatch");
                if(coordinates.key_kind!=kernel::FamiliarityKey::context||
                    coordinates.lookup_key!=parent.location().digest||coordinates.input_cue!=parent.input_cue())
                    throw std::invalid_argument("related recovery link mismatch");
                // The stored selection must still belong to this observation
                // route. An archived general-dialogue reference is not promoted
                // to an observation by restoring its journal.
                auto prepared=restore_saved_cognition(input,metadata);
                if(!kernel::context_reference_eligible(prepared.cognition->replayed.has_input_key(),true))
                    throw std::invalid_argument("saved related original is not an input-bound observation");
                prepared.related_from=parent.location();prepared.related_connection=connection;state.related.emplace(std::move(prepared));
            }else{
                auto recalled=runtime_.related(parent);
                const auto candidate=runtime_.select_replay(recalled,connection?&*connection:nullptr);
                if(!candidate)throw std::invalid_argument("selected original has no related observation");
                parameters=current_parameters(recalled.matches()[*candidate].recalled.recalled_head.identity,parameters);
                auto cognition=runtime_.cognize_selected(recalled,*candidate,parameters.first,parameters.second);
                auto checkpoint=recovery_checkpoint(recalled,cognition.replayed,cognition.comparison);
                Context::RestoredCognition prepared(input,std::move(cognition),parameters,checkpoint,memory_);
                prepared.related_from=parent.location();prepared.related_connection=connection;state.related.emplace(std::move(prepared));
            }
        }
        if(state.related->related_from!=parent.location())throw std::invalid_argument("related parent selection changed");
        complete_saved_cognition(state.related);
        const auto& cognition=*state.related->cognition;
        auto prefix=replay_prefix(cognition.replayed,&cognition,&input);
        constexpr std::string_view content_field=",\"contentHex\":\"";
        prefix.resize(prefix.size()-content_field.size());
        prefix+=",\"related\":true,\"parentCognitionUnchanged\":true,\"relatedFrom\":";
        prefix+=address(parent.location(),memory_);
        if(connection){prefix+=",\"relatedConnection\":\"";prefix+=hex(*connection,memory_);prefix+='"';}
        prefix+=",\"revision\":\"";prefix+=hex(*state.related->revision,memory_);prefix+='"';
        prefix+=content_field;payload_result(id,prefix,evidence_payload(cognition.replayed.original()).content);
    }
    void replay_result(std::string_view id,const Json& p){
        bool related=false;
        if(const auto* flag=p.find("related")){
            if(flag->kind!=Json::Kind::boolean)throw std::invalid_argument("related must be boolean");
            related=flag->scalar=="true";
        }
        std::optional<DigestBytes> related_connection;
        if(const auto* field=p.find("connection")){
            if(!related||!p.find("inputOriginal")||p.find("connections"))
                throw std::invalid_argument("connection Replay requires related/inputOriginal and no listing");
            related_connection=digest(field->string());
        }
        if(p.find("connections")&&(!related||!p.find("inputOriginal")))
            throw std::invalid_argument("connection listing requires related and inputOriginal");
        if(related&&(p.find("scope")||p.find("candidate")||p.find("offset")||p.find("count")))
            throw std::invalid_argument("related cannot combine with scope, candidate or byte range");
        if(context().recovered_cognition && integer(p.at("receipt"))==context().recovered_receipt){
            if(p.find("candidate") || p.find("offset") || p.find("count"))
                throw std::invalid_argument("historical cognition permits only its recorded original");
            const auto metadata=parse_json(content_text(*context().recovered_cognition),memory_);
            if(const auto* expected=p.find("inputOriginal");expected&&
                record_address(*expected)!=record_address(metadata.at("inputOriginal")))
                throw std::invalid_argument("receipt input address mismatch");
            if(const auto* scope=p.find("scope")){
                const auto name=scope->string();if(name.empty())throw std::invalid_argument("scope requires a name");
                const auto input=record_address(metadata.at("inputOriginal"));
                const auto record=runtime_.session().read_latest_cognition(input,input_observation_scope(input.digest,name));
                if(!record)throw std::invalid_argument("recorded scope cognition not found");
                const auto saved=parse_json(content_text(*record),memory_);
                if(record_address(saved.at("inputOriginal"))!=input||saved.at("scope").string()!=name)
                    throw std::invalid_argument("recorded scope cognition binding mismatch");
                std::pmr::string prefix(saved.at("replayPrefix").string(),&memory_);
                constexpr std::string_view content_field=",\"contentHex\":\"";
                if(!prefix.ends_with(content_field))throw std::invalid_argument("invalid recorded scope Replay prefix");
                // Complete only the empty payload field to inspect the sealed
                // checkpoint. Its recorded verdict is not comparison authority.
                const auto checkpoint=parse_json(prefix+"\"}",memory_);
                const auto& tier=checkpoint.at("temporary");
                if(tier.kind!=Json::Kind::boolean)throw std::invalid_argument("invalid recorded scope tier");
                const bool temporary=tier.scalar=="true";
                const auto connection=digest(saved.at("scopeConnection").string());
                const auto* owner=runtime_.session().find(connection);
                if(temporary&&!owner)throw std::invalid_argument("recorded scope owner disappeared");
                auto& state=context();
                if(!state.scoped||state.scoped->input!=input||state.scoped->scope!=name){
                    auto parameters=std::pair{integer(saved.at("seed")),integer(saved.at("step"))};
                    if(owner){
                        const auto latest=owner->latest_refinement_parameters();
                        if(latest.second>=parameters.second)parameters=latest;
                    }else if(const auto* latest=runtime_.main().graph().refinement(connection)){
                        if(latest->current_step()>=parameters.second)parameters={latest->seed(),latest->current_step()};
                    }
                    const auto head=record_address(checkpoint.at("assessment").at("rememberedHead"));
                    const auto index=integer(saved.at("originalIndex"));
                    const auto original=record_address(saved.at("selectedOriginal"));
                    auto cognition=temporary?
                        runtime_.restore_temporary_cognition(input,name,connection,head,index,original,parameters.first,parameters.second):
                        runtime_.restore_main_cognition(input,name,connection,head,record_address(saved.at("observationHead")),
                            index,original,parameters.first,parameters.second);
                    if(cognition.comparison.observation_boundary()!=integer(saved.at("observationBoundary")))
                        throw std::invalid_argument("recorded scope observation boundary mismatch");
                    if(cognition.replayed.source_identity()!=digest(saved.at("sourceSession").string()))
                        throw std::invalid_argument("restored scoped source mismatch");
                    Context::ScopedCognition prepared{input,name,std::move(cognition),parameters,memory_};
                    prepared.temporary=temporary;state.scoped.emplace(std::move(prepared));
                }
                complete_scoped_cognition();
                auto live=scoped_replay_prefix(*state.scoped);
                live.resize(live.size()-content_field.size());
                live+=",\"restored\":true,\"historical\":false,\"revision\":\"";
                live+=hex(*state.scoped->revision,memory_);live+='"';live+=content_field;
                payload_result(id,live,evidence_payload(state.scoped->cognition->replayed.original()).content);return;
            }
            if(metadata.at("selectedOriginal").kind==Json::Kind::null)
                throw std::invalid_argument("recorded cognition had no Replay candidate");
            const auto input=record_address(metadata.at("inputOriginal"));
            auto& state=context();
            if(!state.restored||state.restored->input!=input)
                state.restored.emplace(restore_saved_cognition(input,metadata));
            complete_saved_cognition(state.restored);
            if(related){
                if(const auto* page=p.find("connections"))related_connections_result(id,*page,input,state.restored->cognition->replayed);
                else related_replay_result(id,input,state.restored->cognition->replayed,state.restored->parameters,related_connection);
                return;
            }
            auto prefix=replay_prefix(state.restored->cognition->replayed,&*state.restored->cognition,&input);
            constexpr std::string_view content_field=",\"contentHex\":\"";
            prefix.resize(prefix.size()-content_field.size());
            prefix+=",\"restored\":true,\"historical\":false,\"revision\":\"";
            prefix+=hex(*state.restored->revision,memory_);prefix+='"';prefix+=content_field;
            related_availability(prefix,state.restored->cognition->replayed);
            payload_result(id,prefix,evidence_payload(state.restored->cognition->replayed.original()).content);return;
        }
        if(!context().received||integer(p.at("receipt"))!=context().receipt)throw std::invalid_argument("expired receipt");
        if(const auto* expected=p.find("inputOriginal");expected&&
            record_address(*expected)!=context().received->recorded.original)
            throw std::invalid_argument("receipt input address mismatch");
        if(related){
            const auto& state=context();
            if(!state.cognition)throw std::invalid_argument("input had no parent Replay selection");
            if(const auto* page=p.find("connections")){
                related_connections_result(id,*page,state.received->recorded.original,state.cognition->replayed);return;
            }
            const auto& recorded=state.received->recorded.refinement;
            related_replay_result(id,state.received->recorded.original,state.cognition->replayed,
                {recorded.seed(),recorded.current_step()},related_connection);return;
        }
        if(const auto* scope=p.find("scope")){
            if(p.find("candidate")||p.find("offset")||p.find("count"))
                throw std::invalid_argument("scope cannot be combined with candidate or byte range");
            const auto name=scope->string();
            auto& state=context();
            if(!state.scoped||state.scoped->input!=state.received->recorded.original||state.scoped->scope!=name){
                auto recalled=runtime_.input_scope(state.received->recalled,name);
                const auto& recorded=state.received->recorded.refinement;
                auto cognition=runtime_.cognize(recalled,recorded.seed(),recorded.current_step());
                if(!cognition)throw std::invalid_argument("scope has no Recall candidate");
                Context::ScopedCognition prepared{state.received->recorded.original,name,std::move(*cognition),{recorded.seed(),recorded.current_step()},memory_};
                prepared.temporary=recalled.temporary();state.scoped.emplace(std::move(prepared));
            }
            complete_scoped_cognition();
            const auto& cognition=*state.scoped->cognition;
            auto prefix=scoped_replay_prefix(*state.scoped);
            constexpr std::string_view content_field=",\"contentHex\":\"";
            prefix.resize(prefix.size()-content_field.size());
            prefix+=",\"revision\":\"";prefix+=hex(*state.scoped->revision,memory_);prefix+='"';prefix+=content_field;
            payload_result(id,prefix,evidence_payload(cognition.replayed.original()).content);return;
        }
        const auto* explicit_candidate=p.find("candidate");
        // Default full Replay completes core comparison even when a preceding
        // explicit/partial read cleared cognition. Partial bytes alone never
        // count as an authenticated full Replay for comparison.
        if(!explicit_candidate&&!p.find("offset")&&!p.find("count")&&
           (!context().cognition_done||(!context().native_session.empty()&&!context().cognition_snapshot_saved)))
            complete_cognition();
        // Otherwise export the already compared receipt without another read.
        if(!explicit_candidate && !p.find("offset") && !p.find("count") && context().cognition){
            replay_payload(id,context().cognition->replayed,&*context().cognition);return;
        }
        const auto selected=explicit_candidate ? std::optional<std::size_t>(integer(*explicit_candidate))
            : runtime_.select_replay(context().received->recalled);
        if(!selected)throw std::invalid_argument("Recall has no Replay candidate");
        const auto candidate=*selected;clear_replay();
        if(p.find("offset") || p.find("count")){
            const auto offset=integer(p.at("offset")),count=integer(p.at("count"));
            auto part=runtime_.read_payload_slice(context().received->recalled,candidate,offset,count);
            auto prefix="{\"original\":"+address(part.evidence().original(),memory_)+
                ",\"partial\":true,\"offset\":\""+std::to_string(part.offset()).c_str()+
                "\",\"totalBytes\":\""+std::to_string(part.total_bytes()).c_str()+"\",\"contentHex\":\"";
            payload_result(id,prefix,part.content());return;
        }
        context().replayed.emplace(runtime_.replay(context().received->recalled,candidate));
        replay_payload(id,*context().replayed);
    }
    std::pmr::string call(std::string_view method,const Json& p){
        if(!context().received||integer(p.at("receipt"))!=context().receipt)throw std::invalid_argument("expired receipt");
        if(method=="vrs_re_evidence"){
            if(p.find("scope"))throw std::invalid_argument("refresh a scoped comparison through vrs_replay with scope");
            const auto* replayed=selected_replay();
            if(!replayed)throw std::invalid_argument("Replay required before Re-evidence");
            const auto seed=integer(p.at("seed")),step=integer(p.at("step"));
            auto compared=runtime_.compare_replay(*replayed,seed,step);
            std::optional<ReEvidenceResult> verified;
            if(swegca::architecture::kernel::requires_re_evidence(compared.agreement()))
                verified.emplace(runtime_.re_evidence(*replayed,compared,seed,step));
            const auto& checked=verified ? *verified : compared.evidence();
            auto body=std::pmr::string("{\"agreement\":",&memory_)+std::to_string(static_cast<unsigned>(checked.agreement())).c_str()+",\"status\":"+std::to_string(static_cast<unsigned>(checked.verification().result().verification().judgment().status())).c_str();
            body+=verified ? ",\"reEvidencePerformed\":true" : ",\"reEvidencePerformed\":false";
            body+=",\"replayedOriginal\":";body+=address(checked.replayed_original(),memory_);
            body+=",\"rememberedHead\":";body+=address(checked.remembered_head().record,memory_);
            body+=",\"currentHead\":";body+=address(checked.current_head().record,memory_);
            body+=",\"currentOriginals\":[";bool first=true;
            for(const auto& original:checked.current_originals()){if(!first)body+=',';first=false;body+=address(original,memory_);}
            body+="]}";
            auto& state=context();
            std::optional<DigestBytes> published;
            if(state.cognition&&replayed==&state.cognition->replayed){
                // Publish the successful explicit comparison to the same live
                // receipt only after response construction has succeeded.
                // Already persisted input-time cognition remains historical.
                InputCognition refreshed{state.cognition->candidate,std::move(state.cognition->replayed),
                    std::move(compared),std::move(verified)};
                state.cognition.reset();state.cognition.emplace(std::move(refreshed));
                state.cognition_parameters=std::pair{seed,step};state.cognition_done=true;
                state.cognition_snapshot_saved=false;
                complete_cognition();
                if(!state.native_session.empty())published=state.cognition_revision;
            }else if(!state.native_session.empty()){
                // Persist the actual comparison of the explicitly selected,
                // authenticated Replay without replacing automatic selection.
                auto metadata=std::pmr::string("{\"inputOriginal\":",&memory_)+address(state.received->recorded.original,memory_);
                metadata+=",\"seed\":\"";metadata+=std::to_string(seed);
                metadata+="\",\"step\":\"";metadata+=std::to_string(step);
                metadata+="\",\"sourceSession\":\"";metadata+=hex(replayed->source_identity(),memory_);
                metadata+="\",\"selectedOriginal\":";metadata+=address(replayed->location(),memory_);
                append_recovery_checkpoint(metadata,*replayed,compared);
                metadata+=",\"comparison\":";metadata+=body;metadata+='}';
                published=runtime_.save_cognition_revision(state.received->recorded.original,
                    std::as_bytes(std::span(metadata)));
            }
            if(published){body.pop_back();body+=",\"revision\":\"";body+=hex(*published,memory_);body+="\"}";}
            return body;
        }
        throw std::invalid_argument("unknown tool");
    }
};
}
int main(int argc,char** argv){
    // Every response explicitly flushes at its message boundary. Input must
    // not flush stdout once per byte while receiving a native event frame.
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);
    try{
        if(argc!=4)throw std::invalid_argument("usage: swegca-vrs-mcp create|open|ensure ROOT CONFIG.json");
        std::string_view mode=argv[1];
        const bool limited=mode.starts_with("limited-"),bounded=mode.starts_with("bounded-");
        if(limited||bounded)mode.remove_prefix(8);
        if(mode!="create"&&mode!="open"&&mode!="ensure")throw std::invalid_argument("mode must be create/open/ensure with optional limited prefix");
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
        if(const auto* target=config.find("memoryTargetBytes")){
            const auto value=integer(*target);
            if(value>ram)throw std::invalid_argument("memory target exceeds RAM budget");
            settings.memory_target_bytes=static_cast<std::size_t>(value);
        }
        Server::AutomaticWork automatic;
        if(const auto* work=config.find("automaticWork")){
            if(work->kind!=Json::Kind::object)throw std::invalid_argument("automaticWork must contain seed and step");
            automatic.emplace(integer(work->at("seed")),integer(work->at("step")));
        }
        auto runtime=mode=="ensure"?Runtime::ensure(argv[2],settings,memory):mode=="create"?Runtime::create(argv[2],settings,memory):Runtime::open(argv[2],settings,memory);
        Server server(runtime,memory,frame,automatic);StdioFrames frames(STDIN_FILENO,frame,memory);bool eof=false;
        AgentQuerySocket queries(std::getenv(AgentQuerySocket::environment),memory);
        while(!eof){
            bool background_failure=false;
            try{
                auto line=server.automatic_work()?frames.next(eof,[&]{
                    try{
                        queries.poll([&](std::string_view query){return server.agent_query(query);});
                        const bool work=server.advance_work();return queries.enabled()||work;
                    }catch(...){background_failure=true;throw;}
                }):frames.next(eof);
                if(!eof)server.message(line);
            }catch(const std::bad_alloc&){std::cerr<<"VRS memory budget exhausted\n";return 2;}
            catch(const StdioFrames::ReadError&){std::cerr<<"VRS input failed\n";return 2;}
            catch(const std::exception& e){
                if(background_failure){std::cerr<<"automatic Main work failed: "<<e.what()<<'\n';return 2;}
                server.framing_error();
            }
            if(!std::cout)return 2;
        }
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
