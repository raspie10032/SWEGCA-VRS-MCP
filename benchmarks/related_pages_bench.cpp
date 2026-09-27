#include "vrs/runtime.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <unistd.h>
using namespace swegca::architecture;
using namespace swegca::architecture::kernel;
using namespace swegca::vrs;
int main(int argc,char** argv){
 const auto count=argc>1?std::stoul(argv[1]):256;
 const auto limit=argc>2?std::stoul(argv[2]):64;
 if(!count||count>4096||!limit||limit>64)return 2;
 auto pattern=(std::filesystem::temp_directory_path()/"swegca-related-bench-XXXXXX").string();
 if(!::mkdtemp(pattern.data()))return 2;
 MemoryBudget memory(128<<20);EvidencePolicy policy;policy.axis_count=1;
 DigestBytes owner{};owner[0]=std::byte{1};RuntimeConfig config{owner,policy,1,4<<20,4096,1<<20};
 {
 auto host=Runtime::create(pattern,config,memory);owner[0]=std::byte{2};host.start_session(owner,"bench");
 const std::string text="preserve all requirements";const auto bytes=std::as_bytes(std::span(text));
 const auto input=host.receive({0,0,"bench","user","text/plain",bytes},7,0).recorded.original;
 auto recalled=host.input("text/plain",bytes);auto parent=host.replay(recalled,0);
 EvidenceObservation observation;observation.source=owner;observation.producer=owner;
 for(std::size_t i=0;i<count;++i){observation.observed_at=i+1;
  observation.outcome=i%3==0?EvidenceOutcome::support:i%3==1?EvidenceOutcome::refute:EvidenceOutcome::insufficient;
  (void)host.observe_input_scope(input,std::to_string(i),{i+1,i+1,"bench","tool","text/plain",bytes},observation,7,i+1);
 }
 const auto before=memory.used();std::uint64_t checksum=0;
 for(unsigned i=0;i<8;++i)(void)host.related_connections(parent,limit);
 const auto start=std::chrono::steady_clock::now();
 constexpr unsigned repeats=128;
 for(unsigned i=0;i<repeats;++i){auto page=host.related_connections(parent,limit);checksum+=page.entries.size();}
 const auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();
 if(memory.used()!=before)return 3;
 std::printf("{\"connections\":%zu,\"limit\":%zu,\"iterations\":%u,\"meanNs\":%lld,\"residentBytes\":%zu,\"checksum\":%llu}\n",count,limit,repeats,(long long)(ns/repeats),before,(unsigned long long)checksum);
 }
 std::filesystem::remove_all(pattern);
}
