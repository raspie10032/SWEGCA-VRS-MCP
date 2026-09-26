#include "vrs/runtime.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
using namespace swegca::vrs;
using namespace swegca::architecture;
using Clock=std::chrono::steady_clock;
static Clock::time_point entered;
static unsigned calls=0,reads=0,writes=0;
extern "C" void swegca_recall_entry_probe() noexcept{entered=Clock::now();++calls;}
extern "C" ssize_t __real_pread(int,void*,size_t,off_t);
extern "C" ssize_t __wrap_pread(int fd,void* p,size_t n,off_t o){++reads;return __real_pread(fd,p,n,o);}
extern "C" ssize_t __real_pwrite(int,const void*,size_t,off_t);
extern "C" ssize_t __wrap_pwrite(int fd,const void* p,size_t n,off_t o){++writes;return __real_pwrite(fd,p,n,o);}
DigestBytes id(unsigned n){DigestBytes result{};result[0]=std::byte(n);return result;}
void measure(Runtime& host,std::string_view route,std::string_view media,std::span<const std::byte> content,bool present,bool temporary){
 std::array<std::int64_t,30> samples{};unsigned exceeded=0;
 for(unsigned n=0;n<35;++n){
  const auto previous=calls,r=reads,w=writes;
  const auto began=Clock::now();
  const auto result=host.input(media,content);
  const auto elapsed=std::chrono::duration_cast<std::chrono::nanoseconds>(entered-began).count();
  if(calls!=previous+1||r!=reads||w!=writes||result.familiar()!=present||result.temporary()!=temporary)
   throw std::runtime_error("input boundary probe invariant failed");
  if(n>=5){samples[n-5]=elapsed;exceeded+=elapsed>=1000000;}
 }
 std::sort(samples.begin(),samples.end());
 std::printf("{\"route\":\"%.*s\",\"inputBytes\":%zu,\"samples\":30,\"medianNs\":%lld,\"p95Ns\":%lld,\"maxNs\":%lld,\"atLeast1ms\":%u}\n",
  int(route.size()),route.data(),content.size(),(long long)samples[15],(long long)samples[28],(long long)samples[29],exceeded);
}
int main(){
 const auto base=std::filesystem::temp_directory_path();auto pattern=(base/"swegca-recall-bench-XXXXXX").string();
 if(!::mkdtemp(pattern.data()))throw std::runtime_error("benchmark directory");
 const std::filesystem::path root(pattern);
 MemoryBudget memory(64<<20);EvidencePolicy policy;policy.axis_count=1;
 RuntimeConfig config{id(99),policy,1,8<<20,4096,2<<20};
 const std::array<std::size_t,5> sizes{0,128,4096,65536,1048576};
 std::vector<std::byte> content(sizes.back(),std::byte{65});
 {
  auto host=Runtime::create(root,config,memory);host.start_session(id(1),"prepared");
  for(std::size_t n=0;n<sizes.size();++n)
   (void)host.retain({n,0,"prepared","user","text/plain",std::span(content).first(sizes[n])},7,0);
  for(auto size:sizes)measure(host,"temporary","text/plain",std::span(content).first(size),true,true);
  host.end_session();if(host.work(7,0)!=1)throw std::runtime_error("benchmark Main merge");
  host.start_session(id(2),"active");
  for(auto size:sizes)measure(host,"main","text/plain",std::span(content).first(size),true,false);
  for(auto size:sizes)measure(host,"missing","text/missing",std::span(content).first(size),false,false);
 }
 std::filesystem::remove_all(root);
}
