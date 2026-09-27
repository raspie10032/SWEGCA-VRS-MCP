#include "transport/app_server_wire.hpp"
#include "transport/agent_event_commit.hpp"
#include "vrs/memory_budget.hpp"
#include <cstdio>
using namespace swegca::transport;
int main(){
 for(const bool escaped:{false,true}){
  const std::string text(1<<20,escaped?'\n':'x');
  const auto encoded=quote_json(text,*std::pmr::new_delete_resource());
  const std::string raw=std::string("{\"id\":1,\"method\":\"turn/start\",\"params\":{\"threadId\":\"t\",\"input\":[{\"type\":\"text\",\"text\":")+std::string(encoded)+"}]}}";
  swegca::vrs::MemoryBudget memory(64<<20);AppServerWire wire(memory,1,1);wire.attach("t",0);
  auto delivery=wire.prepare(raw,RpcSender::client,0);
  const auto before=memory.used();
  AgentEventCommit commit(std::string(64,'a'),wire.metadata(delivery,7,0),delivery.event().native_bytes(),"bench",memory);
  std::printf("{\"escaped\":%s,\"nativeBytes\":%zu,\"preparedTrackedBytes\":%zu,\"requestBytes\":%zu,\"retainedTrackedBytes\":%zu,\"peakTrackedBytes\":%zu}\n",escaped?"true":"false",raw.size(),before,commit.request().size(),memory.used(),memory.peak_reserved());
 }
}
