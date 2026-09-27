#include "transport/app_server_wire.hpp"
#include "transport/replay_context.hpp"
#include "transport/input_candidates.hpp"
#include "vrs/memory_budget.hpp"
#include <cstdio>
#include <cstdlib>
using namespace swegca::transport;
static_assert(!swegca::architecture::kernel::accepts_agent_response(RequestTransmission::recorded));
static_assert(swegca::architecture::kernel::accepts_agent_response(RequestTransmission::stream_written));
static_assert(swegca::architecture::kernel::accepts_agent_response(RequestTransmission::recovered_unknown));
static_assert(!swegca::architecture::kernel::accepts_agent_response(static_cast<RequestTransmission>(99)));
static unsigned checks=0;
#define CHECK(x) do{++checks;if(!(x)){std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);std::abort();}}while(false)
template<class F>void rejects(F f){bool failed=false;try{f();}catch(const std::exception&){failed=true;}CHECK(failed);}
static void transmit(AppServerWire& wire,AppServerWire::Delivery& delivery,RpcSender reply_sender=RpcSender::server){
 int sockets[2];CHECK(::socketpair(AF_UNIX,SOCK_STREAM,0,sockets)==0);
 const auto response="{\"id\":"+std::string(delivery.event().fields().at("id").scalar)+",\"result\":{}}";
 rejects([&]{(void)wire.prepare(response,reply_sender,0);});
 wire.bind_socket(delivery,sockets[0]);
 CHECK(!wire.send_ready(delivery)); // JSON without the newline is not a frame.
 rejects([&]{(void)wire.prepare(response,reply_sender,0);});
 CHECK(wire.send_ready(delivery));
 ::close(sockets[0]);::close(sockets[1]);
}
int main(){
 swegca::vrs::MemoryBudget memory(1<<20);
 const std::string a=R"({"id":1,"method":"turn/start","params":{"threadId":"a","input":[{"type":"text","text":"입력"}]}})";
 const std::string b=R"({"id":1,"method":"item/commandExecution/requestApproval","params":{"threadId":"b","unknown":true}})";
 const std::string reply=R"({"id":1,"result":{"original":"reply"}})";

 {
  AppServerWire wire(memory,2,2);wire.attach("a",0);wire.attach("b",4);
  rejects([&]{wire.attach("a",0);});rejects([&]{wire.attach("c",0);});
  rejects([&]{(void)wire.prepare(reply,RpcSender::server,0);});
  rejects([&]{(void)wire.prepare(a,RpcSender::server,0);});
  auto input=wire.prepare(a,RpcSender::client,42);
  CHECK(input.event().session()=="a"&&input.sequence()==0&&input.observed_at()==42&&!input.request_sequence());
  auto params=wire.metadata(input,7,2);
  CHECK(!params.find("native")&&params.at("sequence").string()=="0"&&params.at("observedAt").string()=="42");
  CHECK(!params.find("requestSequence"));
  rejects([&]{(void)wire.forward(input);});CHECK(wire.pending_requests()==0);
  wire.recorded(input);CHECK(wire.forward(input)==a && wire.pending_requests()==1);
  // Restoring the same original cannot promote a known unsent reservation.
  auto original_input=adapt_codex_app_server(a,memory);
  wire.restore_request(RpcSender::client,original_input,0);
  rejects([&]{(void)wire.prepare(reply,RpcSender::server,42);});
  rejects([&]{wire.recorded(input);});
  auto approval=wire.prepare(b,RpcSender::server,43);wire.recorded(approval);
  CHECK(approval.sequence()==4&&wire.pending_requests()==2);
  rejects([&]{(void)wire.prepare(a,RpcSender::client,44);}); // capacity before recording
  rejects([&]{(void)wire.prepare(reply,RpcSender::server,45);});
  transmit(wire,input);transmit(wire,approval,RpcSender::client);
  auto response_a=wire.prepare(reply,RpcSender::server,45);
  auto response_b=wire.prepare(reply,RpcSender::client,46);
  CHECK(response_a.event().session()=="a"&&response_b.event().session()=="b");
  CHECK(response_a.sequence()==1&&response_b.sequence()==5);
  CHECK(response_a.request_sequence()==0&&response_b.request_sequence()==4);
  auto response_params=wire.metadata(response_b,8,3);
  CHECK(response_params.at("requestSequence").string()=="4"&&!response_params.find("native"));
  rejects([&]{(void)wire.forward(response_a);});
  wire.recorded(response_b);CHECK(wire.pending_requests()==1&&wire.forward(response_b)==reply);
  wire.recorded(response_a);CHECK(wire.pending_requests()==0&&wire.forward(response_a)==reply);
  // Completed Delivery objects may outlive their settled request map nodes.
  CHECK(wire.send_ready(input)&&wire.send_ready(approval));
  auto old=wire.prepare(a,RpcSender::client,47);
  auto current=wire.prepare(a,RpcSender::client,48);wire.recorded(current);
  rejects([&]{wire.recorded(old);});rejects([&]{(void)wire.forward(old);});
  AppServerWire foreign(memory,1,1);foreign.attach("a",3);
  rejects([&]{foreign.recorded(old);});rejects([&]{(void)foreign.forward(current);});
 }
 CHECK(memory.used()==0);
 {
  AppServerWire resumed(memory,1,1);resumed.attach("a",7);
  auto original=adapt_codex_app_server(a,memory);
  resumed.restore_request(RpcSender::client,original,4);
  auto response=resumed.prepare(reply,RpcSender::server,100);
  CHECK(response.sequence()==7&&response.request_sequence()==4);
  resumed.recorded(response);CHECK(resumed.pending_requests()==0);
  rejects([&]{resumed.restore_request(RpcSender::client,original,8);});
 }
 CHECK(memory.used()==0);
 {
  AppServerWire wire(memory,1,2);unsigned bindings=0;
  const auto bind=[&](const AgentEvent& event){++bindings;CHECK(event.session()=="a");return 9;};
  const auto started=R"({"method":"thread/started","params":{"thread":{"id":"a"}}})";
  rejects([&]{(void)wire.prepare(a,RpcSender::client,1,bind);});CHECK(bindings==0);
  rejects([&]{(void)wire.prepare(started,RpcSender::client,1,bind);});CHECK(bindings==0);
  auto notice=wire.prepare(started,RpcSender::server,2,bind);CHECK(bindings==1&&notice.sequence()==9);
  wire.recorded(notice);CHECK(wire.forward(notice)==started);
  auto input=wire.prepare(a,RpcSender::client,3,bind);CHECK(bindings==1&&input.sequence()==10);wire.recorded(input);
  auto repeated=wire.prepare(started,RpcSender::server,4,bind);CHECK(bindings==1&&repeated.sequence()==11);wire.recorded(repeated);
  rejects([&]{(void)wire.prepare(R"({"method":"thread/started","params":{"thread":{"id":"b"}}})",RpcSender::server,5,bind);});
  CHECK(bindings==1);
 }
 CHECK(memory.used()==0);
 {
  AppServerWire wire(memory,2,3);
  const auto init=R"({"id":1,"method":"initialize","params":{"clientInfo":{"name":"fixture"}}})";
  rejects([&]{(void)wire.prepare(init,RpcSender::client,1);});
  wire.attach_connection("transport",0);wire.attach("a",0);
  rejects([&]{wire.attach_connection("other",0);});
  auto request=wire.prepare(init,RpcSender::client,1);
  CHECK(request.event().session()=="transport"&&request.sequence()==0);
  rejects([&]{(void)wire.forward(request);});
  wire.recorded(request);CHECK(wire.forward(request)==init&&wire.pending_requests()==1);
  transmit(wire,request);
  auto reply=wire.prepare(R"({"id":1,"result":{}})",RpcSender::server,2);
  CHECK(reply.event().session()=="transport"&&reply.sequence()==1&&reply.request_sequence()==0);
  wire.recorded(reply);CHECK(wire.pending_requests()==0);
  auto ready=wire.prepare(R"({"method":"initialized"})",RpcSender::client,3);
  CHECK(ready.event().session()=="transport"&&ready.sequence()==2);wire.recorded(ready);
  auto input=wire.prepare(a,RpcSender::client,4);
  CHECK(input.event().kind()==swegca::architecture::kernel::AgentEventKind::input&&input.event().session()=="a"&&input.sequence()==0);
  wire.recorded(input);
  for(const auto raw:{R"({"id":2,"method":"turn/start","params":{"input":[]}})",
      R"({"method":"notice","params":{"threadId":"transport"}})",
      R"({"method":"thread/started","params":{"thread":{"id":"transport"}}})",
      R"({"method":"notice","params":{"threadId":null}})"})
   rejects([&]{(void)wire.prepare(raw,RpcSender::server,5);});
 }
 CHECK(memory.used()==0);
 {
  AppServerWire wire(memory,1,2);unsigned bindings=0;
  const auto bind=[&](const AgentEvent& event){++bindings;CHECK(event.session()=="existing");return 8;};
  const auto raw=R"({"id":1,"method":"thread/resume","params":{"threadId":"existing"}})";
  rejects([&]{(void)wire.prepare(raw,RpcSender::server,1,bind);});CHECK(bindings==0);
  auto resumed=wire.prepare(raw,RpcSender::client,1,bind);
  CHECK(bindings==1&&resumed.sequence()==8);wire.recorded(resumed);
  transmit(wire,resumed);
  auto response=wire.prepare(R"({"id":1,"result":{}})",RpcSender::server,2,bind);
  CHECK(response.request_sequence()==8&&response.sequence()==9);wire.recorded(response);
 }
 CHECK(memory.used()==0);
 {
  AppServerWire wire(memory,1,3);wire.attach("a",7);
  auto candidate=wire.prepare(a,RpcSender::client,8);
  wire.preflight(candidate);
  auto original=adapt_codex_app_server(a,memory);
  wire.restore_request(RpcSender::client,original,4);
  rejects([&]{wire.preflight(candidate);});
  rejects([&]{(void)wire.prepare(a,RpcSender::client,8);});
  CHECK(wire.pending_requests()==1);
  // Settling the original frees its ID; failed checks consumed no sequence.
  auto response=wire.prepare(reply,RpcSender::server,9);wire.recorded(response);
  auto retry=wire.prepare(a,RpcSender::client,10);CHECK(retry.sequence()==8);
  wire.preflight(retry);wire.recorded(retry);
 }
 CHECK(memory.used()==0);
 {
  AppServerWire wire(memory,3,3);unsigned bindings=0;
  const auto bind=[&](const AgentEvent& event){++bindings;CHECK(event.kind()==swegca::architecture::kernel::AgentEventKind::content);return 0;};
  const auto read=R"({"id":7,"method":"thread/read","params":{"threadId":"prior"}})";
  auto request=wire.prepare(read,RpcSender::client,1,bind);wire.recorded(request);
  CHECK(bindings==1&&request.event().session()=="prior");
  transmit(wire,request);
  auto response=wire.prepare(R"({"id":7,"result":{}})",RpcSender::server,2,bind);wire.recorded(response);
  CHECK(bindings==1&&response.request_sequence()==0);
  auto notice=wire.prepare(R"({"method":"thread/status/changed","params":{"threadId":"another","status":{"type":"idle"}}})",RpcSender::server,3,bind);
  wire.recorded(notice);CHECK(bindings==2&&notice.event().session()=="another");
  rejects([&]{(void)wire.prepare(a,RpcSender::client,4,bind);});CHECK(bindings==2);
 }
 {
  AppServerWire wire(memory,2,2);wire.attach("a",0);wire.attach("b",0);
  auto input=wire.prepare(a,RpcSender::client,42);
  rejects([&]{wire.include_context(input,"context",1);});
  wire.include_context(input,"recalled data\n한글",4096);
  CHECK(input.event().native_bytes()==a);
  rejects([&]{wire.include_context(input,"second",4096);});
  rejects([&]{(void)wire.forward(input);});
  auto moved=std::move(input);wire.recorded(moved);
  const auto projected=parse_json(wire.forward(moved),memory);
  CHECK(projected.at("id").scalar=="1"&&projected.at("method").string()=="turn/start");
  const auto& params=projected.at("params");CHECK(params.at("threadId").string()=="a");
  CHECK(params.at("input").values.size()==2);
  CHECK(params.at("input").values[0].at("text").string()=="recalled data\n한글");
  CHECK(params.at("input").values[1].at("text").string()=="입력");
  rejects([&]{wire.include_context(moved,"late",4096);});
  auto approval=wire.prepare(b,RpcSender::server,43);
  rejects([&]{wire.include_context(approval,"not input",4096);});
  transmit(wire,moved);
  auto response=wire.prepare(reply,RpcSender::server,44);CHECK(response.request_sequence()==0);
 }
 {
  const auto native=parse_json(R"({"method":"turn/steer","params":{"input":[{"type":"image","url":"local"},{"type":"text","text":"팰월드는 유지\r\nComfyUI만 중지\n"},{"type":"text","text":"if ready, do not remove A."}]}})",memory);
  const auto source=parse_json(R"({"original":{"block":"a","digest":"b","offset":"1","bytes":"2"}})",memory);
  std::pmr::string candidates("{}",&memory);
  append_input_candidates(candidates,native,source,memory,8,4096);
  const auto parsed_candidates=parse_json(candidates,memory);
  const auto& spans=parsed_candidates.at("inputCandidates");
  CHECK(spans.at("candidates").values.size()==3);
  CHECK(spans.at("nonTextItems").string()=="1"&&spans.at("next").kind==Json::Kind::null);
  CHECK(spans.at("semanticVerified").scalar=="false"&&spans.at("requirementsComplete").scalar=="false");
  for(const auto& item:spans.at("candidates").values)CHECK(requirement_matches(requirement_anchor(item),native));
  CHECK(spans.at("candidates").values[0].at("quote").string()=="팰월드는 유지\r\n");
  candidates="{}";append_input_candidates(candidates,native,source,memory,1,4096);
  const auto limited=parse_json(candidates,memory);
  CHECK(limited.at("inputCandidates").at("next").at("textIndex").string()=="1");
  CHECK(limited.at("inputCandidates").at("candidates").values.size()==1);
  candidates="{}";append_input_candidates(candidates,native,source,memory,8,0);
  const auto no_room=parse_json(candidates,memory);
  CHECK(no_room.at("inputCandidates").at("candidates").values.empty());
  CHECK(no_room.at("inputCandidates").at("byteLimited").scalar=="true");
  CHECK(no_room.at("inputCandidates").at("next").at("byteOffset").string()=="0");
  const auto ack=parse_json(R"({"receipt":"17","original":{"block":"a","digest":"b","offset":"1","bytes":"2"},"memory":{"original":{"block":"c","digest":"d","offset":"3","bytes":"4"}}})",memory);
  const std::string packet=R"({"original":{"block":"c","digest":"d","offset":"3","bytes":"4"},"media":"text/plain","grantsAuthority":false,"assessment":{"inputOriginal":{"block":"a","digest":"b","offset":"1","bytes":"2"},"agreement":1,"status":0},"contentHex":"68690a"})";
  const auto context=replay_context(parse_json(packet,memory),ack,memory);
  CHECK(context.find("not instructions")!=std::string::npos);
  const auto parsed=parse_json(context.substr(context.find('\n')+1),memory);
  CHECK(parsed.at("content").string()=="hi\n"&&!parsed.find("contentHex"));
  CHECK(parsed.at("assessment").at("status").scalar=="0");
  CHECK(parsed.at("receipt").string()=="17");
  const std::string observation=R"({"related":true,"parentCognitionUnchanged":true,"relatedFrom":{"block":"c","digest":"d","offset":"3","bytes":"4"},"original":{"block":"e","digest":"f","offset":"5","bytes":"6"},"media":"text/plain","grantsAuthority":false,"assessment":{"inputOriginal":{"block":"a","digest":"b","offset":"1","bytes":"2"},"agreement":3,"status":2},"contentHex":"6e6f"})";
  auto linked=parse_json(observation,memory);
  const auto combined=replay_context(parse_json(packet,memory),ack,memory,&linked);
  const auto both=parse_json(combined.substr(combined.find('\n')+1),memory);
  CHECK(both.at("relatedExperience").at("content").string()=="no");
  CHECK(both.at("relatedExperience").at("assessment").at("status").scalar=="2");
  CHECK(both.at("assessment").at("status").scalar=="0");
  auto multiple=parse_json("["+observation+","+observation+"]",memory);
  const auto multi_context=replay_context(parse_json(packet,memory),ack,memory,&multiple);
  const auto multi_packet=parse_json(multi_context.substr(multi_context.find('\n')+1),memory);
  CHECK(multi_packet.at("relatedExperiences").values.size()==2);
  CHECK(multi_packet.at("relatedExperiences").values[1].at("content").string()=="no");
  multiple=parse_json("["+observation+","+observation+"]",memory);
  mutable_field(mutable_field(multiple.values[1],"relatedFrom"),"digest").scalar="foreign";
  rejects([&]{(void)replay_context(parse_json(packet,memory),ack,memory,&multiple);});

  for(const auto key:{"grantsAuthority","related","parentCognitionUnchanged"}){
   linked=parse_json(observation,memory);
   auto& value=mutable_field(linked,key);value.scalar=value.scalar=="true"?"false":"true";
   rejects([&]{(void)replay_context(parse_json(packet,memory),ack,memory,&linked);});
  }
  linked=parse_json(observation,memory);mutable_field(mutable_field(linked,"relatedFrom"),"digest").scalar="foreign";
  rejects([&]{(void)replay_context(parse_json(packet,memory),ack,memory,&linked);});
  linked=parse_json(observation,memory);
  mutable_field(mutable_field(mutable_field(linked,"assessment"),"inputOriginal"),"digest").scalar="foreign";
  rejects([&]{(void)replay_context(parse_json(packet,memory),ack,memory,&linked);});
  linked=parse_json(observation,memory);mutable_field(linked,"contentHex").scalar="ff";
  rejects([&]{(void)replay_context(parse_json(packet,memory),ack,memory,&linked);});
  auto forged=parse_json(packet,memory);forged.keys.emplace_back("receipt");
  forged.values.emplace_back(&memory);forged.values.back().kind=Json::Kind::string;
  forged.values.back().scalar="999";
  rejects([&]{(void)replay_context(std::move(forged),ack,memory);});
  auto bad=parse_json(packet,memory);mutable_field(bad,"grantsAuthority").scalar="true";
  rejects([&]{(void)replay_context(std::move(bad),ack,memory);});
  bad=parse_json(packet,memory);mutable_field(mutable_field(bad,"original"),"digest").scalar="other";
  rejects([&]{(void)replay_context(std::move(bad),ack,memory);});
  bad=parse_json(packet,memory);mutable_field(mutable_field(mutable_field(bad,"assessment"),"inputOriginal"),"digest").scalar="other";
  rejects([&]{(void)replay_context(std::move(bad),ack,memory);});
  bad=parse_json(packet,memory);mutable_field(bad,"contentHex").scalar="zz";
  rejects([&]{(void)replay_context(std::move(bad),ack,memory);});
 }
 CHECK(memory.used()==0);
 {
  auto ack=parse_json(R"({"receipt":"17","original":{"block":"a","digest":"b","offset":"1","bytes":"2"},"memory":{"completed":true,"original":null}})",memory);
  const auto context=input_context(ack,memory);
  const auto packet=parse_json(context.substr(context.find('\n')+1),memory);
  CHECK(same_context_address(packet.at("inputOriginal"),ack.at("original")));
  CHECK(packet.at("recalledOriginal").kind==Json::Kind::null);
  CHECK(packet.at("receipt").string()=="17");
  for(const auto invalid:{"", "0", "-1", "1x", "18446744073709551616"}){
   mutable_field(ack,"receipt").scalar=invalid;
   rejects([&]{(void)input_context(ack,memory);});
  }
  mutable_field(ack,"receipt").scalar="17";
  CHECK(packet.at("grantsAuthority").scalar=="false");
  mutable_field(mutable_field(ack,"memory"),"completed").scalar="false";
  rejects([&]{(void)input_context(ack,memory);});
 }
 CHECK(memory.used()==0);
 for(const bool empty:{false,true}){
  AppServerWire wire(memory,1,1);wire.attach("a",0);
  const std::string prefix=R"( {"id":9,"method":"turn/start", "params":{"threadId":"a","inpu\u0074":[)";
  const std::string input=empty?"  ":R"( {"type":"text","text":"keep\u0020escape","unknown":1e+09} )";
  const std::string suffix=R"(],"future":{"input":"[not the array]"}}} )";
  const auto native=prefix+input+suffix;
  auto delivery=wire.prepare(native,RpcSender::client,1);
  const std::string inserted=std::string(R"({"type":"text","text":"recall\u000a한글"})")+(empty?"":",");
  const auto exact=native.size()+inserted.size();
  rejects([&]{wire.include_context(delivery,"recall\n한글",exact-1);});
  wire.include_context(delivery,"recall\n한글",exact);wire.recorded(delivery);
  CHECK(wire.forward(delivery)==prefix+inserted+input+suffix);
  CHECK(delivery.event().native_bytes()==native);
 }
 CHECK(memory.used()==0);
 {
  AppServerWire wire(memory,1,1);wire.attach("a",0);
  const auto native=std::string(R"({"id":1,"method":"turn/start","params":{"threadId":"a","input":[{"type":"text","text":")")+
      std::string(1<<18,'x')+R"("}]}})";
  auto delivery=wire.prepare(native,RpcSender::client,1);
  const auto held_bytes=memory.limit()-memory.used()-native.size()-4096;
  auto* held=memory.allocate(held_bytes);
  wire.include_context(delivery,"remembered",native.size()+128);
  memory.deallocate(held,held_bytes);wire.recorded(delivery);
  CHECK(delivery.event().native_bytes()==native&&wire.forward(delivery).size()>native.size());
 }
 CHECK(memory.used()==0);
 std::printf("app-server wire owner tests: %u checks passed\n",checks);
}
