#include "transport/app_server_wire.hpp"
#include "vrs/memory_budget.hpp"
#include <cstdio>
#include <cstdlib>
#include <iostream>
using namespace swegca::transport;
static unsigned checks=0;
#define CHECK(x) do{++checks;if(!(x)){std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);std::abort();}}while(false)
template<class F>void rejects(F f){bool failed=false;try{f();}catch(const std::exception&){failed=true;}CHECK(failed);}
int main(int argc,char** argv){
 swegca::vrs::MemoryBudget memory(1<<20);
 const std::string a=R"({"id":1,"method":"turn/start","params":{"threadId":"a","input":[{"type":"text","text":"입력"}]}})";
 const std::string b=R"({"id":1,"method":"item/commandExecution/requestApproval","params":{"threadId":"b","unknown":true}})";
 const std::string reply=R"({"id":1,"result":{"original":"reply"}})";
 if(argc==2&&std::string_view(argv[1])=="--exchange"){
  AppServerWire wire(memory,2,2);wire.attach("a",0);wire.attach("b",0);
  const auto deliver=[&](std::string_view raw,RpcSender sender){
   auto plan=wire.prepare(raw,sender,42);
   std::cout<<"{\"session\":"<<quote_json(plan.event().session(),memory)
       <<",\"parameters\":"<<wire.parameters(plan,7,0)<<"}\n"<<std::flush;
   std::string ack;if(!std::getline(std::cin,ack)||ack!="recorded")throw std::runtime_error("ingestion not confirmed");
   wire.recorded(plan);
   std::cout<<"{\"forwarded\":"<<quote_json(wire.forward(plan),memory)<<"}\n"<<std::flush;
  };
  deliver(a,RpcSender::client);deliver(b,RpcSender::server);
  deliver(reply,RpcSender::server);deliver(reply,RpcSender::client);
  CHECK(wire.pending_requests()==0);return 0;
 }
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
 std::printf("app-server wire owner tests: %u checks passed\n",checks);
}
