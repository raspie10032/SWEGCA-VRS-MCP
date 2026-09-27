#include "transport/agent_event_commit.hpp"
#include "vrs/memory_budget.hpp"
#include <cstdio>
using namespace swegca::transport;
int main(){
 for(const bool escaped:{false,true}){
  swegca::vrs::MemoryBudget memory(64<<20);
  const std::string identity(64,'a');
  Json fields(&memory);fields.kind=Json::Kind::object;
  Json native(&memory);native.kind=Json::Kind::string;
  native.scalar.assign(1<<20,escaped?'\n':'x');
  const auto input=memory.used();
  AgentEventCommit commit(identity,std::move(fields),native.scalar,"bench",memory);
  std::pmr::string empty(&memory);native.scalar.swap(empty);empty.clear();empty.shrink_to_fit();
  std::printf("{\"escaped\":%s,\"nativeBytes\":1048576,\"inputTrackedBytes\":%zu,\"requestBytes\":%zu,\"retainedTrackedBytes\":%zu,\"peakTrackedBytes\":%zu}\n",
    escaped?"true":"false",input,commit.request().size(),memory.used(),memory.peak_reserved());
 }
}
