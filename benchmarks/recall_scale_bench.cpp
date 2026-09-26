#include "vrs/runtime.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <sys/resource.h>
#include <unistd.h>
using namespace swegca::vrs;
using namespace swegca::architecture;
using Clock=std::chrono::steady_clock;
static Clock::time_point entered;
static unsigned calls=0;
static std::atomic<unsigned> reads{0},writes{0};
extern "C" void swegca_recall_entry_probe() noexcept{entered=Clock::now();++calls;}
extern "C" ssize_t __real_pread(int,void*,size_t,off_t);
extern "C" ssize_t __wrap_pread(int fd,void* p,size_t n,off_t o){++reads;return __real_pread(fd,p,n,o);}
extern "C" ssize_t __real_pwrite(int,const void*,size_t,off_t);
extern "C" ssize_t __wrap_pwrite(int fd,const void* p,size_t n,off_t o){++writes;return __real_pwrite(fd,p,n,o);}
static long long ns(Clock::duration elapsed){return std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();}
static DigestBytes id(unsigned n){DigestBytes d{};d[0]=std::byte(n);return d;}
static void measure(Runtime& host,MemoryBudget& memory,const char* route,std::string_view media,
    std::span<const std::byte> content,std::size_t expected,bool temporary){
 std::array<long long,30> entry{},complete{};std::size_t receipt_bytes=0;unsigned exceeded=0;
 for(unsigned n=0;n<35;++n){
  const auto r=reads.load(),w=writes.load(),c=calls;const auto baseline=memory.used();
  {
   const auto start=Clock::now();const auto receipt=host.input(media,content);const auto end=Clock::now();
   if(reads!=r||writes!=w||calls!=c+1||receipt.matches().size()!=expected||receipt.temporary()!=temporary)
    throw std::runtime_error("scaled Recall invariant");
   receipt_bytes=memory.used()-baseline;
   if(n>=5){entry[n-5]=ns(entered-start);complete[n-5]=ns(end-start);exceeded+=entry[n-5]>=1000000;}
  }
  if(memory.used()!=baseline)throw std::runtime_error("scaled Recall allocation leak");
 }
 std::sort(entry.begin(),entry.end());std::sort(complete.begin(),complete.end());
 std::printf("{\"route\":\"%s\",\"candidates\":%zu,\"samples\":30,\"entryMedianNs\":%lld,\"entryP95Ns\":%lld,\"entryMaxNs\":%lld,\"entryAtLeast1ms\":%u,\"completeMedianNs\":%lld,\"completeP95Ns\":%lld,\"receiptBytes\":%zu,\"trackedBytes\":%zu}\n",
  route,expected,entry[15],entry[28],entry[29],exceeded,complete[15],complete[28],receipt_bytes,memory.used());std::fflush(stdout);
}
int main(int argc,char** argv){
 std::size_t count=2048;
 if(argc!=3&&argc!=4)throw std::invalid_argument("usage: recall-scale-bench COUNT repeated|distinct [WORKERS]");
 std::uint32_t workers=1;
 if(argc==4){const std::string_view value=argv[3];const auto result=std::from_chars(value.data(),value.data()+value.size(),workers);
  if(result.ec!=std::errc{}||result.ptr!=value.data()+value.size()||!workers)throw std::invalid_argument("benchmark worker count");}
 const std::string_view number=argv[1],mode=argv[2];const auto parsed=std::from_chars(number.data(),number.data()+number.size(),count);
 if(parsed.ec!=std::errc{}||parsed.ptr!=number.data()+number.size()||!count||(mode!="repeated"&&mode!="distinct"))throw std::invalid_argument("benchmark arguments");
 auto pattern=(std::filesystem::temp_directory_path()/"swegca-scale-XXXXXX").string();
 if(!::mkdtemp(pattern.data()))throw std::runtime_error("benchmark directory");
 const std::filesystem::path root(pattern);
 struct Cleanup{std::filesystem::path path;~Cleanup(){std::error_code error;std::filesystem::remove_all(path,error);}} cleanup{root};
 MemoryBudget memory(256ULL<<20);EvidencePolicy policy;policy.axis_count=1;
 RuntimeConfig config{id(99),policy,1,8<<20,4096,2<<20};config.merge_workers=workers;
 std::printf("{\"configuration\":{\"workers\":%u}}\n",workers);
 std::array<std::byte,128> content{};std::vector<long long> ingestion;ingestion.reserve(count);
 std::size_t expected=mode=="repeated"?count:1;ExperienceLocation last;
 {
  auto host=Runtime::create(root,config,memory);host.start_session(id(1),"prepared");
  for(std::size_t n=0;n<count;++n){
   if(mode=="distinct")for(unsigned byte=0;byte<8;++byte)content[byte]=std::byte((std::uint64_t(n)>>(byte*8))&255);
   const auto start=Clock::now();auto event=host.receive({n,n,"prepared","user","application/octet-stream",content},7,n);
   ingestion.push_back(ns(Clock::now()-start));last=event.recorded.original;
   if(event.recalled.matches().size()!=(mode=="repeated"?n:0))throw std::runtime_error("ingestion Recall count");
  }
  std::sort(ingestion.begin(),ingestion.end());
  std::printf("{\"phase\":\"recorded\",\"mode\":\"%.*s\",\"experiences\":%zu,\"receiveMedianNs\":%lld,\"receiveP95Ns\":%lld,\"receiveMaxNs\":%lld,\"trackedBytes\":%zu,\"storedBytes\":%llu}\n",int(mode.size()),mode.data(),count,ingestion[count/2],ingestion[(count-1)*95/100],ingestion.back(),memory.used(),(unsigned long long)host.storage().used());std::fflush(stdout);
  measure(host,memory,"temporary-exact","application/octet-stream",content,expected,true);
  {auto found=host.input("application/octet-stream",content);if(host.replay(found,expected-1).location()!=last)throw std::runtime_error("temporary selected original");}
  measure(host,memory,"temporary-continuation","text/followup",content,expected,true);
  host.end_session();const auto start=Clock::now();if(host.work(7,count)!=1)throw std::runtime_error("Main merge count");
  std::printf("{\"phase\":\"merged\",\"elapsedNs\":%lld,\"trackedBytes\":%zu}\n",ns(Clock::now()-start),memory.used());std::fflush(stdout);
  host.start_session(id(2),"active");
  measure(host,memory,"main-exact","application/octet-stream",content,expected,false);
  {auto found=host.input("application/octet-stream",content);if(host.replay(found,expected-1).location()!=last)throw std::runtime_error("Main selected original");}
  measure(host,memory,"main-continuation","text/followup",content,expected,false);
 }
 if(memory.used())throw std::runtime_error("owner allocation leak");
 const auto start=Clock::now();
 {auto host=Runtime::open(root,config,memory);host.resume_session(id(2));
  std::printf("{\"phase\":\"reopened\",\"elapsedNs\":%lld,\"trackedBytes\":%zu}\n",ns(Clock::now()-start),memory.used());std::fflush(stdout);
  measure(host,memory,"reopened-main-exact","application/octet-stream",content,expected,false);
 }
 if(memory.used())throw std::runtime_error("reopened owner allocation leak");
 rusage usage{};if(::getrusage(RUSAGE_SELF,&usage))throw std::runtime_error("getrusage");
 std::printf("{\"phase\":\"finished\",\"maxRssKiB\":%ld}\n",usage.ru_maxrss);
}
