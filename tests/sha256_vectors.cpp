#include "swegca_architecture/sha256.hpp"
#include <cstdio>
#include <stdexcept>
#include <vector>
using swegca::architecture::Sha256;
int main(){
 std::vector<std::byte> storage((1<<20)+4);
 for(std::size_t i=0;i<storage.size();++i)storage[i]=std::byte((i*131+(i>>8))&255);
 std::vector<std::size_t> lengths;
 for(std::size_t n=0;n<=257;++n)lengths.push_back(n);
 for(auto n:{4095,4096,65535,65536,1048576})lengths.push_back(n);
 for(unsigned offset=0;offset<4;++offset)for(auto length:lengths){
  const auto data=std::span(storage).subspan(offset,length);
  const auto expected=Sha256::of(data);
  for(auto stride:{1U,7U,63U,64U,65U,1000U}){
   Sha256 streamed;
   for(std::size_t at=0;at<data.size();at+=stride)streamed.update(data.subspan(at,std::min<std::size_t>(stride,data.size()-at)));
   if(streamed.finish()!=expected)throw std::runtime_error("SHA streaming mismatch");
  }
  std::printf("%u %zu ",offset,length);
  for(auto byte:expected)std::printf("%02x",std::to_integer<unsigned>(byte));
  std::puts("");
 }
}
