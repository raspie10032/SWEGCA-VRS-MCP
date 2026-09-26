#include "vrs/runtime.hpp"
#include "transport/agent_event.hpp"
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
using namespace swegca::vrs;
using namespace swegca::architecture;
using namespace swegca::architecture::kernel;
namespace fs=std::filesystem;
static unsigned checks=0,reads=0,writes=0;
#define CHECK(x) do{++checks;if(!(x)){std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);std::abort();}}while(false)
template<class E,class F>void rejects(F f){bool caught=false;try{f();}catch(const E&){caught=true;}CHECK(caught);}
extern "C" ssize_t __real_pread(int,void*,size_t,off_t);
extern "C" ssize_t __wrap_pread(int fd,void* p,size_t n,off_t at){++reads;return __real_pread(fd,p,n,at);}
extern "C" ssize_t __real_pwrite(int,const void*,size_t,off_t);
extern "C" ssize_t __wrap_pwrite(int fd,const void* p,size_t n,off_t at){++writes;return __real_pwrite(fd,p,n,at);}
int main(){
 auto pattern=(fs::temp_directory_path()/"swegca-multi-session-XXXXXX").string();CHECK(::mkdtemp(pattern.data()));const fs::path root(pattern);
 MemoryBudget memory(64<<20);EvidencePolicy policy;policy.axis_count=1;
 const auto key=[](std::string_view provider){return swegca::transport::agent_session_identity(provider,"host","same-native-id");};
 const auto a=key("codex"),b=key("another-agent"),c=key("third-agent"),main_id=key("Main");
 RuntimeConfig config{main_id,policy,1,65536,4096,8192};config.merge_workers=2;
 const std::string text="same bytes from different agents",unique="only published source";
 const auto bytes=std::as_bytes(std::span(text)),other=std::as_bytes(std::span(unique));
 ExperienceLocation original_a,original_b,published;
 {
  auto host=Runtime::create(root,config,memory);host.start_session(a,"a");
  original_a=host.retain({0,0,"a","user","text/plain",bytes},7,0).original;
  auto recalled_a=host.input("text/plain",bytes);
  const auto* a_session=&host.session();
  host.attach_session(b,"b");CHECK(host.attached_sessions()==2 && &host.session()==a_session);
  rejects<std::logic_error>([&]{host.attach_session(b,"b");});
  rejects<std::invalid_argument>([&]{host.select_session(c);});CHECK(&host.session()==a_session);
  auto r=reads,w=writes;host.select_session(b);CHECK(reads==r && writes==w);
  CHECK(!host.input("text/plain",bytes).familiar());
  r=reads;w=writes;
  rejects<std::invalid_argument>([&]{(void)host.replay(recalled_a,0);});
  rejects<std::invalid_argument>([&]{(void)host.read_payload_slice(recalled_a,0,0,1);});
  CHECK(reads==r && writes==w);
  original_b=host.retain({0,0,"b","user","text/plain",bytes},7,0).original;
  auto recalled_b=host.input("text/plain",bytes);CHECK(recalled_b.temporary() && recalled_b.matches().size()==1);
  CHECK(host.read_payload_slice(recalled_b,0,0,1).evidence().original()==original_b);
  host.select_session(a);CHECK(&host.session()==a_session && host.read_payload_slice(recalled_a,0,0,1).evidence().original()==original_a);
  CHECK(host.work(7,0)==0 && host.main().graph().generation()==0);
  host.attach_session(c,"c");host.select_session(c);
  published=host.retain({0,0,"c","tool","text/plain",other},7,0).original;
  host.end_session();CHECK(!host.has_session() && host.attached_sessions()==2);
  CHECK(host.main().graph().generation()==0);CHECK(host.work(7,0)==1);
  CHECK(host.main().graph().source_count()==1);
  // Both detached routes received the published Main index, even without a
  // selected session during work. Neither active session was ended or merged.
  for(auto identity:{a,b}){
   host.select_session(identity);CHECK(host.session().phase()==SessionPhase::active);
   auto recalled=host.input("text/plain",other);
   CHECK(!recalled.temporary() && recalled.matches().size()==1);
   CHECK(host.replay(recalled,0).location()==published);
   auto local=host.input("text/plain",bytes);CHECK(local.temporary() && local.matches().size()==1);
   CHECK(host.replay(local,0).location()==(identity==a?original_a:original_b));
  }
  // Destruction with two attached sessions is not either session's end.
 }
 CHECK(memory.used()==0);
 {
  auto host=Runtime::open(root,config,memory);CHECK(host.work(7,0)==0);
  host.attach_resumed_session(a);host.attach_resumed_session(b);
  CHECK(!host.has_session() && host.attached_sessions()==2);
  host.select_session(a);CHECK(host.session().phase()==SessionPhase::active);
  CHECK(host.replay(host.input("text/plain",bytes),0).location()==original_a);
  host.select_session(b);auto old=host.input("text/plain",bytes);
  CHECK(host.replay(old,0).location()==original_b);
  host.end_session();CHECK(host.attached_sessions()==1 && !host.has_session());
  host.select_session(a);const auto* stable=&host.session();CHECK(host.work(7,0)==1);
  CHECK(&host.session()==stable && host.session().phase()==SessionPhase::active);
  rejects<std::invalid_argument>([&]{(void)host.replay(old,0);});
  CHECK(host.main().graph().source_count()==2);
  CHECK(host.replay(host.input("text/plain",bytes),0).location()==original_a);
  host.end_session();CHECK(host.attached_sessions()==0);CHECK(host.work(7,0)==1);
  CHECK(host.main().graph().source_count()==3);
 }
 CHECK(memory.used()==0);fs::remove_all(root);
 std::printf("multi-session runtime tests: %u checks passed\n",checks);
}
