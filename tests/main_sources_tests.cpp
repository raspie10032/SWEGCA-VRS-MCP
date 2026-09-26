#include "vrs/main_sources.hpp"
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
using namespace swegca::architecture;
using namespace swegca::architecture::kernel;
using namespace swegca::vrs;
namespace fs=std::filesystem;
static unsigned checks=0,reads=0;
#define CHECK(e) do{++checks;if(!(e)){std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#e);std::abort();}}while(false)
template<class E,class F>void throws(F f){bool caught=false;try{f();}catch(const E&){caught=true;}CHECK(caught);}
extern "C" ssize_t __real_pread(int,void*,size_t,off_t);
extern "C" ssize_t __wrap_pread(int fd,void* p,size_t n,off_t offset){++reads;return __real_pread(fd,p,n,offset);}
DigestBytes id(unsigned n){DigestBytes d{};d[0]=std::byte(n);return d;}
std::string filename(unsigned n){constexpr char digits[]="0123456789abcdef";std::string s(64,'0');s[0]=digits[n>>4];s[1]=digits[n&15];return s+".session";}
int main(){
 auto pattern=(fs::temp_directory_path()/"swegca-main-sources-XXXXXX").string();CHECK(::mkdtemp(pattern.data()));fs::path root(pattern);
 MemoryBudget memory(64<<20);EvidencePolicy policy;policy.axis_count=1;
 ExperienceLocation a_original,b_original,c_original,head;double strength=0;
 {
  auto live=SessionStore::create(root,id(3),"live",16384,memory);
  auto ended=SessionStore::create(root,id(4),"ended",16384,memory);ended.end();
  for(unsigned n:{1u,2u}){
   auto store=SessionStore::create(root,id(n),"source",16384,memory);SessionRuntime runtime(store,memory,8192);
   runtime.define_connection(id(20),1,policy);
   EvidenceObservation observation;observation.hypothesis=id(20);observation.source=id(n);observation.context=id(n);observation.producer=id(n);
   observation.outcome=n==1?EvidenceOutcome::support:EvidenceOutcome::refute;
   const auto value=runtime.observe(id(20),{0,0,"source","producer","text/plain",{}},observation,7,0).original;
   if(n==1)a_original=value;else b_original=value;
   runtime.end();runtime.publish_originals();
  }
  MainSources sources(root,memory,8192);
  CHECK(sources.published()==std::pmr::vector<DigestBytes>({id(1),id(2)},&memory));
  throws<std::runtime_error>([&]{(void)sources.resolve(id(3));});
  throws<std::runtime_error>([&]{(void)sources.resolve(id(4));});
  CHECK(live.phase()==SessionPhase::active&&ended.phase()==SessionPhase::ended);
  auto main=PersistentMainGraph::create(root/"graph",id(99),memory,1,policy,1024);
  CHECK(sources.merge_published(main,7,0)==2);
  CHECK(main.graph().source_count()==2&&main.graph().generation()==2);
  sources.release_caches();
  CHECK(main.graph().replay(id(20),0).location()==a_original);
  CHECK(main.graph().replay(id(20),1).location()==b_original);
  const auto before=reads;CHECK(sources.merge_published(main,8,1)==0);CHECK(reads==before);
  head=main.head();strength=main.graph().find(id(20))->strength();
  // Caches can be recreated without losing originals or repeating a merge.
  CHECK(sources.resolve(id(1)).find(id(20))->state().experiences()[0].original()==a_original);
  sources.release_caches();CHECK(sources.merge_published(main,7,0)==0);
 }
 CHECK(memory.used()==0);
 {
  MainSources sources(root,memory,8192);
  auto main=PersistentMainGraph::open(root/"graph",id(99),memory,1,policy,sources);
  sources.release_caches();
  CHECK(main.head()==head&&main.graph().find(id(20))->strength()==strength);
  CHECK(main.graph().replay(id(20),1).location()==b_original);
  auto store=SessionStore::open(root,id(3),memory);SessionRuntime temporary(store,memory,8192);
  ExperienceRouter router(temporary,memory);router.mount_main(main);
  auto recalled=router.input("text/plain",{});CHECK(recalled.matches().size()==2);
  CHECK(router.replay(recalled,0).location()==a_original);
  temporary.define_connection(id(20),1,policy);
  EvidenceObservation current;current.hypothesis=id(20);current.source=id(3);current.context=id(3);current.producer=id(3);
  c_original=temporary.observe(id(20),{0,0,"live","producer","text/plain",{}},current,7,0).original;
  CHECK(sources.published().size()==2);
  // Explicit end and publication are both necessary before discovery.
  temporary.end();CHECK(sources.published().size()==2);temporary.publish_originals();
  CHECK(sources.published().size()==3);
  // A published source still has exactly one store owner.
  throws<std::system_error>([&]{(void)sources.resolve(id(3));});
  const auto alias=root/"main"/filename(5);fs::create_symlink(root/"main"/filename(1),alias);
  throws<std::runtime_error>([&]{(void)sources.published();});fs::remove(alias);
  fs::create_hard_link(root/"main"/filename(1),root/"main"/"bad.session");
  throws<std::runtime_error>([&]{(void)sources.published();});fs::remove(root/"main"/"bad.session");
 }
 CHECK(memory.used()==0);
 {
  MainSources sources(root,memory,8192);
  auto main=PersistentMainGraph::open(root/"graph",id(99),memory,1,policy,sources);
  CHECK(sources.merge_published(main,9,0)==1);
  CHECK(main.graph().source_count()==3&&main.graph().generation()==3);
  CHECK(main.graph().replay(id(20),2).location()==c_original);
  CHECK(sources.merge_published(main,9,0)==0);
 }
 CHECK(memory.used()==0);
 {
  // Missing recorded source prevents graph recovery, instead of silently
  // replacing it with a newer or unrelated session.
  fs::rename(root/"main"/filename(2),root/"retained-publication");
  MainSources sources(root,memory,8192);
  throws<std::runtime_error>([&]{(void)PersistentMainGraph::open(root/"graph",id(99),memory,1,policy,sources);});
 }
 CHECK(memory.used()==0);fs::remove_all(root);
 std::printf("Main source discovery tests: %u checks passed\n",checks);
}
