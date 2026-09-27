#include "transport/app_server_wire.hpp"
#include "vrs/memory_budget.hpp"
#include <cstdio>
#include <cstdlib>
using namespace swegca::transport;
static unsigned checks=0;
#define CHECK(x) do{++checks;if(!(x)){std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);std::abort();}}while(false)
template<class F>void rejects(F f){bool failed=false;try{f();}catch(const std::exception&){failed=true;}CHECK(failed);}
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
  auto params=parse_json(wire.parameters(input,7,2),memory);
  CHECK(params.at("native").string()==a&&params.at("sequence").string()=="0"&&params.at("observedAt").string()=="42");
  CHECK(!params.find("requestSequence"));
  rejects([&]{(void)wire.forward(input);});CHECK(wire.pending_requests()==0);
  wire.recorded(input);CHECK(wire.forward(input)==a && wire.pending_requests()==1);
  rejects([&]{wire.recorded(input);});
  auto approval=wire.prepare(b,RpcSender::server,43);wire.recorded(approval);
  CHECK(approval.sequence()==4&&wire.pending_requests()==2);
  rejects([&]{(void)wire.prepare(a,RpcSender::client,44);}); // capacity before recording
  auto response_a=wire.prepare(reply,RpcSender::server,45);
  auto response_b=wire.prepare(reply,RpcSender::client,46);
  CHECK(response_a.event().session()=="a"&&response_b.event().session()=="b");
  CHECK(response_a.sequence()==1&&response_b.sequence()==5);
  CHECK(response_a.request_sequence()==0&&response_b.request_sequence()==4);
  auto response_params=parse_json(wire.parameters(response_b,8,3),memory);
  CHECK(response_params.at("requestSequence").string()=="4"&&response_params.at("native").string()==reply);
  rejects([&]{(void)wire.forward(response_a);});
  wire.recorded(response_b);CHECK(wire.pending_requests()==1&&wire.forward(response_b)==reply);
  wire.recorded(response_a);CHECK(wire.pending_requests()==0&&wire.forward(response_a)==reply);
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
  auto response=wire.prepare(R"({"id":7,"result":{}})",RpcSender::server,2,bind);wire.recorded(response);
  CHECK(bindings==1&&response.request_sequence()==0);
  auto notice=wire.prepare(R"({"method":"thread/status/changed","params":{"threadId":"another","status":{"type":"idle"}}})",RpcSender::server,3,bind);
  wire.recorded(notice);CHECK(bindings==2&&notice.event().session()=="another");
  rejects([&]{(void)wire.prepare(a,RpcSender::client,4,bind);});CHECK(bindings==2);
 }
 CHECK(memory.used()==0);
 std::printf("app-server wire owner tests: %u checks passed\n",checks);
}
