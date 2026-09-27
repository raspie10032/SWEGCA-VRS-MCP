#include "swegca_architecture/input_cue.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <vector>
int main(){
 std::vector<std::byte> data(1048576,std::byte{65});
 for(const std::size_t size:{0U,128U,4096U,65536U,1048576U}){
  const auto iterations=std::max<std::size_t>(8,1048576/std::max<std::size_t>(1,size));
  std::array<double,17> samples{};
  for(auto& sample:samples){
   const auto start=std::chrono::steady_clock::now();
   for(std::size_t i=0;i<iterations;++i){
    const auto value=swegca::architecture::input_cue("text/plain",std::span(data).first(size));
    asm volatile("" : : "m"(value) : "memory");
   }
   sample=std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-start).count()/iterations;
  }
  std::sort(samples.begin(),samples.end());
  std::printf("%zu median_ns=%.3f min_ns=%.3f max_ns=%.3f\n",size,samples[8],samples[0],samples[16]);
 }
}
