#include "transport/json.hpp"
#include "vrs/memory_budget.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
int main(){
 for(bool unicode:{false,true}){
  std::string text="{\"text\":\"";
  if(unicode)while(text.size()<(1U<<20))text+="한글🙂\\n";
  else text+=std::string(1U<<20,'x');
  text+="\"}";
  swegca::vrs::MemoryBudget memory(16<<20);std::array<long long,31> samples;
  std::size_t total=0;
  for(auto& sample:samples){
   const auto start=std::chrono::steady_clock::now();
   for(unsigned n=0;n<10;++n){auto encoded=swegca::transport::quote_json(text,memory);total+=encoded.size();}
   sample=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count()/10;
  }
  std::sort(samples.begin(),samples.end());
  std::printf("unicode=%d bytes=%zu median_ns=%lld checksum=%zu\n",unicode,text.size(),samples[15],total);
 }
}
