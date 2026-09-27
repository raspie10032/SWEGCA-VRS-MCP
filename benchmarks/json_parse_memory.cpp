#include "transport/json.hpp"
#include "vrs/memory_budget.hpp"
#include <chrono>
#include <cstdio>
using namespace swegca::transport;
int main(){
 for(const bool escaped:{false,true}){
  const std::string raw=escaped?std::string("{\"text\":\"")+std::string(1<<20,'x')+"\"}":std::string(1<<20,'x');
  swegca::vrs::MemoryBudget encoding(16<<20);
  auto input=quote_json(raw,encoding);
  swegca::vrs::MemoryBudget memory(16<<20);
  const auto start=std::chrono::steady_clock::now();
  {
   auto parsed=parse_json(input,memory);
   if(parsed.string()!=raw)return 1;
   const auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();
   std::printf("{\"escaped\":%s,\"decodedBytes\":%zu,\"retainedBytes\":%zu,\"peakBytes\":%zu,\"elapsedNs\":%lld}\n",
     escaped?"true":"false",raw.size(),memory.used(),memory.peak_reserved(),static_cast<long long>(ns));
  }
  if(memory.used())return 2;
 }
}
