#include "transport/app_server_requests.hpp"
#include "vrs/memory_budget.hpp"
#include <cstdio>
#include <cstdlib>
using namespace swegca::transport;
using namespace swegca::architecture::kernel;
static unsigned checks=0;
#define CHECK(x) do{++checks;if(!(x)){std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);std::abort();}}while(false)
template<class F>void rejects(F f){bool failed=false;try{f();}catch(const std::exception&){failed=true;}CHECK(failed);}
int main(){
 swegca::vrs::MemoryBudget memory(1<<20);
 const auto request=[&](std::string_view id,std::string_view thread){
  const auto raw="{\"id\":"+std::string(id)+",\"method\":\"turn/start\",\"params\":{\"threadId\":\""+std::string(thread)+"\",\"input\":[]}}";
  return adapt_codex_app_server(raw,memory);
 };
 {
  AppServerRequests bindings(memory,3);
  auto a=request("1","a"),b=request("1","b"),c=request("\"1\"","c");
  bindings.track(RpcSender::client,a);bindings.track(RpcSender::server,b);bindings.track(RpcSender::client,c);
  CHECK(bindings.pending()==3);
  bindings.track(RpcSender::client,a);CHECK(bindings.pending()==3);
  rejects([&]{bindings.track(RpcSender::client,b);});
  auto fourth=request("2","d");rejects([&]{bindings.track(RpcSender::client,fourth);});
  auto from_server=bindings.bind(R"({"id":1,"result":{"unknown":"keep"}})",RpcSender::server);
  auto from_client=bindings.bind(R"({"id":1,"error":{"code":-1,"message":"failed","data":{"retain":true}}})",RpcSender::client);
  auto string_id=bindings.bind(R"({"id":"1","result":null})",RpcSender::server);
  CHECK(from_server.event().session()=="a" && from_client.event().session()=="b" && string_id.event().session()=="c");
  CHECK(from_server.event().kind()==AgentEventKind::content && from_server.event().native_name().empty());
  CHECK(from_server.event().native_bytes()==R"({"id":1,"result":{"unknown":"keep"}})");
  CHECK(bindings.pending()==3); // Binding is not successful VRS ingestion.
  auto retry=bindings.bind(from_server.event().native_bytes(),RpcSender::server);
  bindings.recorded(from_server);CHECK(bindings.pending()==2);
  bindings.track(RpcSender::client,fourth);CHECK(bindings.pending()==3);
  rejects([&]{bindings.recorded(retry);});
  bindings.recorded(string_id);bindings.track(RpcSender::client,a);
  rejects([&]{bindings.recorded(from_server);}); // Reused ID, different generation.
  AppServerRequests other(memory,1);other.track(RpcSender::client,a);
  rejects([&]{other.recorded(retry);});
  bindings.recorded(from_client);
  for(auto raw:{R"({"id":42,"result":{}})",R"({"id":1,"result":{},"error":{}})",
      R"({"id":1})",R"({"id":1,"method":"x","result":{}})",
      R"({"id":true,"result":{}})",R"({"id":1,"error":{"code":1}})",
      R"({"id":1.0,"result":{}})",R"({"id":9223372036854775808,"result":{}})"})
   rejects([&]{(void)bindings.bind(raw,RpcSender::server);});
  rejects([&]{(void)bindings.bind(R"({"id":2,"result":{}})",RpcSender::client);});
  CHECK(bindings.pending()==2);
 }
 CHECK(memory.used()==0);
 // Reconstruct at exactly the same address: an old response must not gain the
 // new owner's authority even if request ID and generation coincide.
 {
  alignas(AppServerRequests) std::byte storage[sizeof(AppServerRequests)];
  auto a=request("1","a");
  auto* first=new(storage) AppServerRequests(memory,1);first->track(RpcSender::client,a);
  auto old=first->bind(R"({"id":1,"result":{}})",RpcSender::server);
  first->~AppServerRequests();
  auto* second=new(storage) AppServerRequests(memory,1);second->track(RpcSender::client,a);
  rejects([&]{second->recorded(old);});CHECK(second->pending()==1);
  second->~AppServerRequests();
 }
 CHECK(memory.used()==0);
 {
  AppServerRequests bindings(memory,2);
  auto min=request("-9223372036854775808","min"),max=request("9223372036854775807","max");
  bindings.track(RpcSender::client,min);bindings.track(RpcSender::client,max);
  auto low=bindings.bind(R"({"id":-9223372036854775808,"result":null})",RpcSender::server);
  auto high=bindings.bind(R"({"id":9223372036854775807,"result":null})",RpcSender::server);
  CHECK(low.event().session()=="min" && high.event().session()=="max");
  rejects([&]{(void)bindings.bind(R"({"id":-9223372036854775809,"result":null})",RpcSender::server);});
 }
 CHECK(memory.used()==0);
 {
  auto a=request("1","a");swegca::vrs::MemoryBudget tiny(1);
  AppServerRequests bindings(tiny,1);rejects([&]{bindings.track(RpcSender::client,a);});
  CHECK(bindings.pending()==0 && tiny.used()==0);
 }
 CHECK(memory.used()==0);
 {
  swegca::vrs::MemoryBudget budget(16384);AppServerRequests bindings(budget,1);
  auto a=request("1","a");bindings.track(RpcSender::client,a);
  const auto bytes=16384-budget.used();void* held=budget.allocate(bytes);
  rejects([&]{(void)bindings.bind(R"({"id":1,"result":{"data":"still pending"}})",RpcSender::server);});
  CHECK(bindings.pending()==1);budget.deallocate(held,bytes);
  auto good=bindings.bind(R"({"id":1,"result":null})",RpcSender::server);
  bindings.recorded(good);CHECK(bindings.pending()==0);
 }
 CHECK(memory.used()==0);
 rejects([&]{AppServerRequests zero(memory,0);});
 std::printf("app-server request binding tests: %u checks passed\n",checks);
}
