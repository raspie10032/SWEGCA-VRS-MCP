#include "swegca_architecture/region_partition_kernel.hpp"
#include <chrono>
#include <cstdio>
#include <cstdint>
[[gnu::noinline]] std::size_t partition(std::size_t begin,std::size_t total,std::size_t capacity){
 const auto end=swegca::architecture::kernel::region_partition_end(begin,total,capacity);return end?*end:0;
}
int main(){
 constexpr std::uint64_t count=2000000;std::uint64_t checksum=0;
 const auto begin=std::chrono::steady_clock::now();
 for(std::uint64_t n=0;n<count;++n)checksum+=partition(n&1023,1000,(n&7)?256:0);
 const auto elapsed=std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-begin).count();
 std::printf("region partition kernel: %.3f ns/call; calls=%llu checksum=%llu\n",elapsed/count,
  static_cast<unsigned long long>(count),static_cast<unsigned long long>(checksum));
}
