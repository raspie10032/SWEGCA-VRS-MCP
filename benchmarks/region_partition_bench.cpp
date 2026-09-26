#include "swegca_architecture/region_partition_kernel.hpp"
#include "swegca_architecture/recall_route_kernel.hpp"
#include <chrono>
#include <cstdio>
#include <cstdint>
[[gnu::noinline]] std::size_t partition(std::size_t begin,std::size_t total,std::size_t capacity){
 const auto end=swegca::architecture::kernel::region_partition_end(begin,total,capacity);return end?*end:0;
}
[[gnu::noinline]] bool receipt(std::size_t count,std::size_t bytes,std::size_t pin){
 return swegca::architecture::kernel::recall_range_receipt(count,bytes,pin);
}
int main(){
 constexpr std::uint64_t count=2000000;std::uint64_t checksum=0;
 const auto begin=std::chrono::steady_clock::now();
 for(std::uint64_t n=0;n<count;++n)checksum+=partition(n&1023,1000,(n&7)?256:0);
 const auto elapsed=std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-begin).count();
 std::printf("region partition kernel: %.3f ns/call; calls=%llu checksum=%llu\n",elapsed/count,
  static_cast<unsigned long long>(count),static_cast<unsigned long long>(checksum));
 const auto receipt_begin=std::chrono::steady_clock::now();checksum=0;
 for(std::uint64_t n=0;n<count;++n)checksum+=receipt(n&1023,320,(n&7)?24:0);
 const auto receipt_elapsed=std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-receipt_begin).count();
 std::printf("Recall receipt kernel: %.3f ns/call; calls=%llu checksum=%llu\n",receipt_elapsed/count,
  static_cast<unsigned long long>(count),static_cast<unsigned long long>(checksum));
}
