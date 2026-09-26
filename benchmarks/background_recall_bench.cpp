#include "vrs/runtime.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>
using namespace swegca::vrs;
using namespace swegca::architecture;
using Clock=std::chrono::steady_clock;
static std::atomic<bool> running{false},started{false};
static thread_local Clock::time_point entered;
static thread_local unsigned calls=0,reads=0,writes=0;
extern "C" void swegca_background_work_probe(bool value) noexcept{running.store(value);if(value)started.store(true);}
extern "C" void swegca_recall_entry_probe() noexcept{entered=Clock::now();++calls;}
extern "C" ssize_t __real_pread(int,void*,size_t,off_t);
extern "C" ssize_t __wrap_pread(int fd,void* p,size_t n,off_t o){++reads;return __real_pread(fd,p,n,o);}
extern "C" ssize_t __real_pwrite(int,const void*,size_t,off_t);
extern "C" ssize_t __wrap_pwrite(int fd,const void* p,size_t n,off_t o){++writes;return __real_pwrite(fd,p,n,o);}
static DigestBytes id(unsigned n){DigestBytes value{};value[0]=std::byte(n);return value;}
static long long ns(Clock::duration value){return std::chrono::duration_cast<std::chrono::nanoseconds>(value).count();}
static void measure(Runtime& runtime,const char* route,std::string_view media,std::span<const std::byte> content,bool familiar,bool temporary){
 std::array<long long,30> overlapped{},other{};std::size_t count=0,finished=0;unsigned violations=0;
 struct Slow {long long wall,cpu,voluntary,involuntary;};std::array<Slow,30> slow{};
 for(unsigned n=0;n<35;++n){
  const auto before=calls,r=reads,w=writes;const bool live=running.load();
  rusage before_usage{},after_usage{};timespec before_cpu{},after_cpu{};
  if(::getrusage(RUSAGE_THREAD,&before_usage)||::clock_gettime(CLOCK_THREAD_CPUTIME_ID,&before_cpu))throw std::runtime_error("thread timing");
  const auto start=Clock::now();auto found=runtime.input(media,content);
  const auto elapsed=ns(entered-start);const bool still_live=running.load();
  if(::clock_gettime(CLOCK_THREAD_CPUTIME_ID,&after_cpu)||::getrusage(RUSAGE_THREAD,&after_usage))throw std::runtime_error("thread timing");
  if(calls!=before+1||r!=reads||w!=writes||found.familiar()!=familiar||found.temporary()!=temporary)
   throw std::runtime_error("background Recall boundary invariant");
  if(n<5)continue;
  if(live&&still_live){overlapped[count++]=elapsed;if(elapsed>=1000000){
   slow[violations++]={elapsed,(after_cpu.tv_sec-before_cpu.tv_sec)*1000000000LL+after_cpu.tv_nsec-before_cpu.tv_nsec,
     after_usage.ru_nvcsw-before_usage.ru_nvcsw,after_usage.ru_nivcsw-before_usage.ru_nivcsw};}}
  else other[finished++]=elapsed;
 }
 std::sort(overlapped.begin(),overlapped.begin()+count);std::sort(other.begin(),other.begin()+finished);
 std::printf("{\"route\":\"%s\",\"inputBytes\":%zu,\"overlappedSamples\":%zu,\"entryMedianNs\":%lld,\"entryP95Ns\":%lld,\"entryMaxNs\":%lld,\"entryAtLeast1ms\":%u,\"nonOverlappedSamples\":%zu,\"nonOverlappedMaxNs\":%lld}\n",
  route,content.size(),count,count?overlapped[count/2]:-1,count?overlapped[(count-1)*95/100]:-1,count?overlapped[count-1]:-1,violations,finished,finished?other[finished-1]:-1);
 for(unsigned n=0;n<violations;++n)std::printf("{\"slowSample\":\"%s\",\"inputBytes\":%zu,\"entryWallNs\":%lld,\"threadCpuThroughReturnNs\":%lld,\"voluntarySwitches\":%lld,\"involuntarySwitches\":%lld}\n",
  route,content.size(),slow[n].wall,slow[n].cpu,slow[n].voluntary,slow[n].involuntary);
}
int main(){
 auto pattern=(std::filesystem::temp_directory_path()/"swegca-background-recall-XXXXXX").string();
 if(!::mkdtemp(pattern.data()))throw std::runtime_error("benchmark directory");
 const std::filesystem::path root(pattern);
 struct Cleanup{std::filesystem::path path;~Cleanup(){std::error_code error;std::filesystem::remove_all(path,error);}} cleanup{root};
 MemoryBudget memory(64<<20);EvidencePolicy policy;policy.axis_count=1;
 RuntimeConfig config{id(99),policy,1,8<<20,4096,2<<20,2};
 const std::array<std::size_t,5> sizes{0,128,4096,65536,1048576};
 std::vector<std::byte> content(sizes.back(),std::byte{65});ExperienceLocation selected;
 {
  auto runtime=Runtime::create(root,config,memory);runtime.start_session(id(1),"baseline");
  for(std::size_t n=0;n<sizes.size();++n){auto saved=runtime.retain({n,0,"baseline","user","text/main",std::span(content).first(sizes[n])},7,0);if(n==4)selected=saved.original;}
  runtime.end_session();if(runtime.work(7,0)!=1)throw std::runtime_error("baseline merge");
  runtime.start_session(id(2),"pending");
  for(unsigned n=0;n<2048;++n)(void)runtime.retain({n,n,"pending","tool","text/pending",std::span(content).first(128)},7,n);
  runtime.end_session(); // Leave this ended source unmerged for cold recovery.
 }
 if(memory.used())throw std::runtime_error("setup allocation leak");
 {
  auto runtime=Runtime::open(root,config,memory);runtime.start_session(id(3),"active");
  for(std::size_t n=0;n<sizes.size();++n)(void)runtime.retain({n,0,"active","user","text/temporary",std::span(content).first(sizes[n])},7,0);
  const auto begin=Clock::now();if(!runtime.schedule_work(7,2048))throw std::runtime_error("missing scheduled source");
  const auto deadline=Clock::now()+std::chrono::seconds(10);
  while(!started.load()&&Clock::now()<deadline)std::this_thread::yield();
  if(!started.load())throw std::runtime_error("background worker did not start");
  for(auto size:sizes){
   const auto input=std::span(content).first(size);
   measure(runtime,"main-during-prepare","text/main",input,true,false);
   measure(runtime,"temporary-during-prepare","text/temporary",input,true,true);
   measure(runtime,"missing-during-prepare","text/missing",input,false,false);
  }
  if(runtime.main().graph().generation()!=1)throw std::runtime_error("worker published without owner");
  while(running.load()&&Clock::now()<deadline)std::this_thread::yield();
  if(running.load())throw std::runtime_error("background preparation timeout");
  const auto commit_begin=Clock::now();std::optional<std::size_t> merged;
  while(!(merged=runtime.poll_work())&&Clock::now()<deadline)std::this_thread::yield();
  if(!merged||*merged!=1)throw std::runtime_error("background publication count");
  std::printf("{\"phase\":\"published\",\"pendingExperiences\":2048,\"elapsedNs\":%lld,\"pollCommitNs\":%lld,\"trackedBytes\":%zu}\n",ns(Clock::now()-begin),ns(Clock::now()-commit_begin),memory.used());
  const auto recalled=runtime.input("text/main",content);
  if(runtime.replay(recalled,0).location()!=selected)throw std::runtime_error("selected original changed");
 }
 if(memory.used())throw std::runtime_error("background owner allocation leak");
 rusage usage{};if(::getrusage(RUSAGE_SELF,&usage))throw std::runtime_error("getrusage");
 std::printf("{\"phase\":\"finished\",\"maxRssKiB\":%ld}\n",usage.ru_maxrss);
}
