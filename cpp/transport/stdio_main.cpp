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
    return std::pmr::string("{\"status\":",&memory)+std::to_string(static_cast<unsigned>(judgment.status())).c_str()+
        ",\"reason\":"+std::to_string(static_cast<unsigned>(judgment.reason())).c_str()+
        ",\"strength\":"+decimal(report.result().strength().current(),memory)+
        ",\"revision\":\""+std::to_string(report.after_revision()).c_str()+"\"}";
}
constexpr std::string_view tools_list=R"({"tools":[
{"name":"vrs_replay","description":"Read one original from current Recall. Without candidate, SWEGCA selects by stored connection strength, recency and stable address. Optional offset and count return only that verified byte range, without a completed Replay receipt for Re-evidence. Does not infer truth or authorize actions.","inputSchema":{"type":"object","properties":{"receipt":{"type":"string"},"candidate":{"type":"string"},"offset":{"type":"string","description":"Raw payload byte offset; requires count."},"count":{"type":"string","description":"Byte count; requires offset."}},"required":["receipt"],"additionalProperties":false}},
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
        if(automatic_&&runtime_.work_scheduled())(void)runtime_.poll_work();
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
            if(method.starts_with("swegca/")){auto body=host(method,request.at("params"),line,native_source);result(encoded_id,body);return;}
            error(encoded_id,-32601,"unknown method");
        }catch(const std::invalid_argument& e){if(id)error(encoded_id,-32602,e.what());}
        catch(const std::exception& e){if(id)error(encoded_id,-32000,e.what());}
    }
    void framing_error(){error("null",-32700,"invalid or oversized MCP frame");}
private:
    Runtime& runtime_;MemoryBudget& memory_;std::uint64_t frame_;bool initialized_=false,ready_=false;
    AutomaticWork automatic_;
    bool work_requested_=false;
    struct Context {
        struct Delivery { DigestBytes fingerprint; ExperienceLocation original; DigestBytes context{}; ExperienceSender sender=ExperienceSender::unspecified; };
        Context(MemoryBudget& memory,std::string_view native,bool app,bool connection):deliveries(&memory),native_session(native,&memory),app_server(app),connection_scope(connection){}
        std::pmr::map<std::uint64_t,Delivery> deliveries;
        bool native_ready=false;
        std::pmr::string native_session;
        bool app_server=false;
        bool connection_scope=false;
        std::string_view native_source() const noexcept{return connection_scope?"codex/app-server-connection":app_server?"codex/app-server":"codex/hook";}
        std::uint64_t receipt=0;
        std::optional<ReceivedInput> received;
        std::optional<ReplayedInput> replayed;
        std::optional<InputCognition> cognition;
        bool cognition_done=false,cognition_saved=false;
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
        std::cout<<"{\"jsonrpc\":\"2.0\",\"id\":"<<id<<",\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"";
        write_json_string_content(std::cout,prefix);write_json_hex(std::cout,bytes);
        std::cout<<"\\\"}\"}],\"structuredContent\":"<<prefix;
        write_json_hex(std::cout,bytes);std::cout<<"\"}}}\n"<<std::flush;
        if(!std::cout)throw std::runtime_error("MCP output disconnected");
    }
    void tool_result(std::string_view id,std::string_view body){
        // The verified result is already materialized. Emit its two required
        // MCP representations without allocating escaped/combined duplicates.
        std::cout<<"{\"jsonrpc\":\"2.0\",\"id\":"<<id<<",\"result\":{\"content\":[{\"type\":\"text\",\"text\":";
        write_json_string(std::cout,body);
        std::cout<<"}],\"structuredContent\":"<<body<<"}}\n"<<std::flush;
        if(!std::cout)throw std::runtime_error("MCP output disconnected");
    }
    void result(std::string_view id,std::string_view body){std::cout<<"{\"jsonrpc\":\"2.0\",\"id\":"<<id<<",\"result\":"<<body<<"}\n"<<std::flush;if(!std::cout)throw std::runtime_error("MCP output disconnected");}
    void error(std::string_view id,int code,std::string_view message){std::cout<<"{\"jsonrpc\":\"2.0\",\"id\":"<<id<<",\"error\":{\"code\":"<<code<<",\"message\":"<<quote_json(message,memory_)<<"}}\n"<<std::flush;}
    void clear_replay(){
        context().replayed.reset();context().cognition.reset();context().cognition_done=false;context().cognition_saved=false;
    }
    void clear(){clear_replay();context().cognition_parameters.reset();context().received.reset();context().recovered_cognition.reset();context().recovered_receipt=0;}
    const ReplayedInput* selected_replay() const {
        if(context().replayed)return &*context().replayed;
        return context().cognition ? &context().cognition->replayed : nullptr;
    }
    void invalidate_cognition(const RecordedRefinement& recorded){
        auto& state=context();
        if(state.cognition&&state.cognition->assessment().remembered_head().identity==recorded.refinement.connection()){
            state.cognition_parameters=std::pair{recorded.refinement.seed(),recorded.refinement.current_step()};
            state.cognition_done=false;state.cognition_saved=false;
        }
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
            state.cognition_done=true;
        }
        if(!state.native_session.empty() && !state.cognition_saved){
            auto metadata=std::pmr::string("{\"inputOriginal\":",&memory_)+address(state.received->recorded.original,memory_);
            cognition_body(metadata);
            if(state.cognition){
                metadata+=",\"sourceSession\":\"";metadata+=hex(state.cognition->replayed.source_identity(),memory_);
                metadata+="\",\"selectedOriginal\":";metadata+=address(state.cognition->replayed.location(),memory_);
                metadata+=",\"replayPrefix\":";metadata+=quote_json(replay_prefix(state.cognition->replayed,&*state.cognition),memory_);
            }else metadata+=",\"sourceSession\":null,\"selectedOriginal\":null,\"replayPrefix\":null";
            metadata+='}';
            runtime_.save_cognition(state.received->recorded.original,std::as_bytes(std::span(metadata)));
            state.cognition_saved=true;
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
                    Context::Delivery{delivery.fingerprint(),delivery.original(),delivery.context(),delivery.sender()});
                if(!inserted && (at->second.fingerprint!=delivery.fingerprint() || at->second.original!=delivery.original()))
                    throw std::invalid_argument("conflicting stored native sequence");
            });
            if(!state.deliveries.empty() && (state.deliveries.begin()->first!=0 ||
               state.deliveries.rbegin()->first!=state.deliveries.size()-1))
                throw std::invalid_argument("noncontiguous stored native sequence");
            state.native_ready=true;
            return "{\"identity\":\""+hex(identity,memory_)+"\",\"nextSequence\":\""+std::to_string(state.deliveries.size()).c_str()+"\"}";
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
                const bool input=request_event.kind()==swegca::architecture::kernel::AgentEventKind::input;
                const auto cue=input?request_event.cue_content():request_event.native_bytes();
                request_connection=input_cue(input?request_event.cue_media():"application/json",std::as_bytes(std::span(cue)));
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
                std::pmr::string body("{\"duplicate\":true,\"original\":",&memory_);
                body+=address(found->second.original,memory_);body+=",\"receipt\":";
                if(state.received && state.cognition_done && state.received->recorded.original==found->second.original){
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
                    }else if(state.received && state.received->recorded.original==found->second.original){
                        complete_cognition();body+='"';body+=std::to_string(state.receipt);body+='"';cognition_body(body);
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
                    state.received.emplace(runtime_.receive_envelope(event.cue_media(),std::as_bytes(std::span(prompt)),original,seed,step));
                    slot->second.original=state.received->recorded.original;committed=true;
                    slot->second.fingerprint=agent_delivery_identity(sequence,observed,event.native_bytes());
                    complete_cognition();
                    return received_body(limit);
                }
                auto recorded=[&]{
                    if(!request_original)return runtime_.retain(original,seed,step);
                    EvidenceObservation observation;observation.hypothesis=request_connection;
                    observation.context=request_original->digest;observation.observed_at=observed;
                    Sha256 source;source.update("SWEGCA input source v1");source.update(original.source);
                    observation.source=observation.producer=source.finish();
                    return runtime_.observe(request_connection,original,observation,seed,step);
                }();
                slot->second.original=recorded.original;committed=true;
                if(request_original){slot->second.context=request_original->digest;binding->recorded(*response);}
                slot->second.fingerprint=agent_delivery_identity(sequence,observed,event.native_bytes());
                return "{\"original\":"+address(recorded.original,memory_)+",\"refinement\":"+refinement(recorded.refinement,memory_)+"}";
            } catch(...) {if(!committed)state.deliveries.erase(slot);throw;}

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
            if(!context().native_session.empty())throw std::invalid_argument("native session requires agent event ingress");
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
                try {
                    auto recorded=runtime_.retain({sequence,observed,session,source,media,content},seed,step);
                    invalidate_cognition(recorded);
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
                auto& state=context();
                const bool affects_cognition=state.cognition&&
                    state.cognition->assessment().remembered_head().identity==value.hypothesis;
                // A failed write may poison the session after a durable prefix.
                // Never allow the cached assessment to bypass that failure.
                if(affects_cognition){state.cognition_done=false;state.cognition_saved=false;}
                auto recorded=runtime_.observe(value.hypothesis,{sequence,observed,session,source,media,content},value,seed,step);
                invalidate_cognition(recorded);
                return "{\"original\":"+address(recorded.original,memory_)+",\"refinement\":"+refinement(recorded.refinement,memory_)+"}";
            }
            if(next_receipt_==std::numeric_limits<std::uint64_t>::max())throw std::overflow_error("receipt sequence exhausted");
            clear();context().receipt=++next_receipt_;context().received.emplace(runtime_.receive({sequence,observed,session,source,media,content},seed,step));
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
    std::pmr::string replay_prefix(const ReplayedInput& replayed,const InputCognition* cognition=nullptr){
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
            prefix+="\",\"inputOriginal\":";prefix+=address(context().received->recorded.original,memory_);
            prefix+=",\"rememberedHead\":";prefix+=address(assessment.remembered_head().record,memory_);
            prefix+=",\"currentHead\":";prefix+=address(assessment.current_head().record,memory_);
            prefix+=",\"currentOriginalCount\":\"";prefix+=std::to_string(assessment.current_originals().size());
            prefix+="\",\"reEvidencePerformed\":";prefix+=cognition->reverified?"true":"false";prefix+='}';
        }else prefix+="null";
        prefix+=",\"contentHex\":\"";
        return prefix;
    }
    void replay_payload(std::string_view id,const ReplayedInput& replayed,const InputCognition* cognition=nullptr){
        payload_result(id,replay_prefix(replayed,cognition),evidence_payload(replayed.original()).content);
    }
    void replay_result(std::string_view id,const Json& p){
        if(context().recovered_cognition && integer(p.at("receipt"))==context().recovered_receipt){
            if(p.find("candidate") || p.find("offset") || p.find("count"))
                throw std::invalid_argument("historical cognition permits only its recorded original");
            const auto metadata=parse_json(content_text(*context().recovered_cognition),memory_);
            if(metadata.at("selectedOriginal").kind==Json::Kind::null)
                throw std::invalid_argument("recorded cognition had no Replay candidate");
            const auto location=record_address(metadata.at("selectedOriginal"));
            auto original=runtime_.read_cognition_original(digest(metadata.at("sourceSession").string()),location);
            payload_result(id,metadata.at("replayPrefix").string(),evidence_payload(original).content);
            return;
        }
        if(!context().received||integer(p.at("receipt"))!=context().receipt)throw std::invalid_argument("expired receipt");
        const auto* explicit_candidate=p.find("candidate");
        // A committed observation for this connection invalidates only the
        // cached assessment. Refresh before export while retaining the original.
        if(!explicit_candidate&&!p.find("offset")&&!p.find("count")&&context().cognition&&!context().cognition_done)
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
            if(state.cognition&&replayed==&state.cognition->replayed){
                // Publish the successful explicit comparison to the same live
                // receipt only after response construction has succeeded.
                // Already persisted input-time cognition remains historical.
                InputCognition refreshed{state.cognition->candidate,std::move(state.cognition->replayed),
                    std::move(compared),std::move(verified)};
                state.cognition.reset();state.cognition.emplace(std::move(refreshed));
                state.cognition_parameters=std::pair{seed,step};state.cognition_done=true;
            }
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
        while(!eof){
            bool background_failure=false;
            try{
                auto line=server.automatic_work()?frames.next(eof,[&]{
                    try{return server.advance_work();}catch(...){background_failure=true;throw;}
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
