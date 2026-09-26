#include "vrs/runtime.hpp"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
using namespace swegca::architecture;
using namespace swegca::architecture::kernel;
using namespace swegca::vrs;
namespace fs=std::filesystem;
static unsigned checks=0,reads=0,writes=0;static bool fail_write=false;
#define CHECK(e) do{++checks;if(!(e)){std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#e);std::abort();}}while(false)
template<class E,class F>void throws(F f){bool caught=false;try{f();}catch(const E&){caught=true;}CHECK(caught);}
extern "C" ssize_t __real_pread(int,void*,size_t,off_t);
extern "C" ssize_t __wrap_pread(int fd,void* p,size_t n,off_t o){++reads;return __real_pread(fd,p,n,o);}
extern "C" ssize_t __real_pwrite(int,const void*,size_t,off_t);
extern "C" ssize_t __wrap_pwrite(int fd,const void* p,size_t n,off_t o){++writes;if(fail_write){fail_write=false;errno=ENOSPC;return -1;}return __real_pwrite(fd,p,n,o);}
DigestBytes id(unsigned n){DigestBytes d{};d[0]=std::byte(n);return d;}
int main(){
 auto pattern=(fs::temp_directory_path()/"swegca-runtime-XXXXXX").string();CHECK(::mkdtemp(pattern.data()));const fs::path root(pattern);
 MemoryBudget memory(64<<20);EvidencePolicy policy;policy.axis_count=1;
 RuntimeConfig config{id(99),policy,1,16384,1024,8192};
 const std::string text="retain every original";const auto content=std::as_bytes(std::span(text));
 ExperienceLocation first,second;
 {
  auto host=Runtime::create(root,config,memory);
  CHECK(!host.has_session());throws<std::logic_error>([&]{(void)host.input("text/plain",content);});
  host.start_session(id(1),"one");
  throws<std::logic_error>([&]{host.start_session(id(2),"two");});
  auto before=host.input("text/plain",content);CHECK(!before.familiar());
  first=host.retain({0,0,"one","user","text/plain",content},7,0).original;
  const auto r=reads,w=writes;auto recalled=host.input("text/plain",content);
  CHECK(recalled.temporary()&&reads==r&&writes==w);
  CHECK(host.replay(recalled,0).location()==first);
  CHECK(host.work(7,0)==0&&host.main().graph().generation()==0);
  // Destroying the runtime is not an explicit end event.
 }
 CHECK(memory.used()==0);
 {
  auto host=Runtime::open(root,config,memory);
  CHECK(host.work(7,0)==0);host.resume_session(id(1));
  CHECK(host.session().phase()==SessionPhase::active);
  CHECK(host.replay(host.input("text/plain",content),0).location()==first);
  host.end_session();CHECK(!host.has_session()&&host.main().graph().generation()==0);
  throws<std::logic_error>([&]{host.end_session();});
  // Exit with a published session queued but no graph merge yet.
 }
 CHECK(memory.used()==0);
 {
  auto host=Runtime::open(root,config,memory);host.start_session(id(2),"two");
  CHECK(!host.input("text/plain",content).familiar());
  CHECK(host.work(7,0)==1); // also refreshes the active session's Main index
  auto recalled=host.input("text/plain",content);CHECK(!recalled.temporary()&&recalled.matches().size()==1);
  CHECK(host.replay(recalled,0).location()==first);
  second=host.retain({0,0,"two","assistant","text/plain",content},7,0).original;
  CHECK(host.input("text/plain",content).temporary());
  CHECK(host.work(7,0)==0&&host.main().graph().generation()==1);
  host.end_session();
  // A merge write failure leaves the published source available for recovery.
  fail_write=true;throws<std::system_error>([&]{(void)host.work(7,0);});
  CHECK(!host.main().usable());
 }
 CHECK(memory.used()==0);
 {
  auto host=Runtime::open(root,config,memory);CHECK(host.work(7,0)==1);
  CHECK(host.main().graph().generation()==2);CHECK(host.work(8,1)==0);
  host.start_session(id(3),"three");auto recalled=host.input("text/plain",content);
  CHECK(recalled.matches().size()==2&&!recalled.temporary());
  CHECK(host.replay(recalled,1).location()==second);
  CHECK(host.re_evidence(host.replay(recalled,0),9,0).agreement()==ReplayAgreement::insufficient);
 }
 CHECK(memory.used()==0);
 {
  // Recover a session that was explicitly ended before its publication call.
  auto store=SessionStore::open(root,id(3),memory);store.end();
 }
 {
  auto host=Runtime::open(root,config,memory);host.resume_session(id(3));
  CHECK(host.session().phase()==SessionPhase::ended);
  throws<std::logic_error>([&]{(void)host.retain({0,0,"three","user","text/plain",content},7,0);});
  host.end_session();CHECK(!host.has_session());CHECK(host.work(7,0)==0); // no experiences
 }
 CHECK(memory.used()==0);fs::remove_all(root);std::printf("runtime lifecycle tests: %u checks passed\n",checks);
}
