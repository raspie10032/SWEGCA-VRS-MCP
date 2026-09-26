#include "transport/resource_profile.hpp"
#include <cstdio>
#include <string_view>
#ifdef __linux__
#include <sys/mman.h>
#endif
int main(int argc,char** argv){
 try {
  if(argc<2)return 2;
  const std::string_view mode=argv[1];
  if(mode=="verify"||mode=="oom")
   swegca::transport::launch_resource_profile(mode,"/","/dev/null",64ULL<<20,"6 7");
  swegca::transport::verify_resource_profile(64ULL<<20,"6 7");
  std::puts("verified memory.max=67108864 memory.swap.max=0 CPUs=6,7");std::fflush(stdout);
  if(mode=="bounded-verify")return 0;
#ifdef __linux__
  if(mode=="bounded-oom") {
   auto* raw=::mmap(nullptr,128ULL<<20,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
   if(raw==MAP_FAILED)return 3;
   std::ifstream group("/proc/self/cgroup");std::string membership;std::getline(group,membership);
   std::printf("cgroup=%s\n",membership.c_str());
   std::puts("touching 128MiB; expected to be killed within the isolated 64MiB group");std::fflush(stdout);
   auto* data=static_cast<volatile unsigned char*>(raw);
   for(std::size_t i=0;i<(128ULL<<20);i+=4096)data[i]=1;
   ::munmap(raw,128ULL<<20);return 4; // Survival means enforcement failed.
  }
#endif
  return 2;
 }catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 1;}
}
