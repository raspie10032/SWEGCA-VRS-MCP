#include "vrs/persistent_main_graph.hpp"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>

using namespace swegca::architecture;
using namespace swegca::architecture::kernel;
using namespace swegca::vrs;
namespace fs=std::filesystem;
static unsigned checks=0;
#define CHECK(e) do{++checks;if(!(e)){std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#e);std::abort();}}while(false)
template<class E,class F>void throws(F f){bool caught=false;try{f();}catch(const E&){caught=true;}CHECK(caught);}
static int writes_left=-1;static bool fail_sync=false;
extern "C" ssize_t __real_pwrite(int,const void*,size_t,off_t);
extern "C" ssize_t __wrap_pwrite(int fd,const void* data,size_t n,off_t pos){
 if(writes_left==0){writes_left=-1;errno=ENOSPC;return -1;}if(writes_left>0)--writes_left;return __real_pwrite(fd,data,n,pos);
}
extern "C" int __real_fdatasync(int);
extern "C" int __wrap_fdatasync(int fd){if(fail_sync){fail_sync=false;errno=EIO;return -1;}return __real_fdatasync(fd);}
DigestBytes id(unsigned n){DigestBytes d{};for(unsigned i=0;i<4;++i)d[i]=std::byte(n>>(8*i));return d;}
class Resolver:public MainSourceResolver {
public:std::map<DigestBytes,const SessionRuntime*> sources;unsigned calls=0;
 const SessionRuntime& resolve(const DigestBytes& identity)override{++calls;return *sources.at(identity);}
};
void fill(SessionRuntime& runtime,std::string_view name,unsigned base,EvidenceOutcome outcome){
 for(unsigned n=0;n<8;++n){EvidenceObservation v;v.hypothesis=id(10);v.source=id(base+n);v.context=id(base+n+1000);v.producer=id(base+n+2000);v.outcome=outcome;
  (void)runtime.observe(id(10),{n,0,name,"experiment","text/plain",{}},v,7,0);
 }
 runtime.end();runtime.publish_originals();
}
int main(){
 auto pattern=(fs::temp_directory_path()/"swegca-persistent-main-XXXXXX").string();CHECK(::mkdtemp(pattern.data()));const fs::path root(pattern);
 MemoryBudget memory(64<<20);EvidencePolicy policy;policy.axis_count=1;
 {
  auto a_store=SessionStore::create(root,id(1),"a",65536,memory);SessionRuntime a(a_store,memory,8192);a.define_connection(id(10),0.75,policy);fill(a,"a",100,EvidenceOutcome::support);
  auto b_store=SessionStore::create(root,id(2),"b",65536,memory);SessionRuntime b(b_store,memory,8192);b.define_connection(id(10),0.75,policy);fill(b,"b",200,EvidenceOutcome::refute);
  auto c_store=SessionStore::create(root,id(3),"c",65536,memory);SessionRuntime c(c_store,memory,8192);c.define_connection(id(10),0.75,policy);fill(c,"c",300,EvidenceOutcome::insufficient);
  Resolver resolver;resolver.sources={{id(1),&a},{id(2),&b},{id(3),&c}};
  const auto path=root/"main-graph";const auto identity=id(77);ExperienceLocation first_head,last_head;double strength=0;
  {
   auto main=PersistentMainGraph::create(path,identity,memory,1,policy,1024);
   CHECK(main.graph().generation()==0);
   throws<std::system_error>([&]{(void)PersistentMainGraph::open(path,identity,memory,1,policy,resolver);});
   CHECK(main.merge(a,17,0));first_head=main.head();CHECK(main.graph().generation()==1);
   CHECK(!main.merge(a,18,1)&&main.head()==first_head);
   writes_left=1;throws<std::system_error>([&]{(void)main.merge(b,19,0);});
   CHECK(!main.usable());throws<std::logic_error>([&]{(void)main.graph();});
  }
  const auto old_size=fs::file_size(path/"m-0000000000000000.block");
  {
   auto main=PersistentMainGraph::open(path,identity,memory,1,policy,resolver);
   CHECK(main.graph().generation()==1&&main.head()==first_head);
   CHECK(main.merge(b,19,0));CHECK(main.graph().generation()==2);
   CHECK(fs::file_size(path/"m-0000000000000000.block")==old_size);
   CHECK(fs::exists(path/"m-0000000000000001.block"));
   // Complete record present, but sync did not report success: reopen decides
   // from actual validated storage rather than assuming the old memory state.
   fail_sync=true;throws<std::system_error>([&]{(void)main.merge(c,23,0);});CHECK(!main.usable());
  }
  {
   auto main=PersistentMainGraph::open(path,identity,memory,1,policy,resolver);
   CHECK(main.graph().generation()==3&&main.graph().source_count()==3);
   CHECK(!main.merge(c,999,1));CHECK(main.graph().find(id(10))->experiences().size()==24);
   CHECK(main.graph().replay(id(10),0).location()==a.find(id(10))->state().experiences()[0].original());
   CHECK(main.graph().replay(id(10),8).location()==b.find(id(10))->state().experiences()[0].original());
   strength=main.graph().find(id(10))->strength();last_head=main.head();
  }
  {
   auto main=PersistentMainGraph::open(path,identity,memory,1,policy,resolver);
   CHECK(main.graph().find(id(10))->strength()==strength&&main.head()==last_head);
  }
  auto changed=policy;changed.accept_margin=0.1;
  throws<std::runtime_error>([&]{(void)PersistentMainGraph::open(path,identity,memory,1,changed,resolver);});
  throws<std::runtime_error>([&]{(void)PersistentMainGraph::open(path,id(78),memory,1,policy,resolver);});
  throws<std::runtime_error>([&]{(void)PersistentMainGraph::open(path,identity,memory,0.5,policy,resolver);});
  Resolver missing;throws<std::out_of_range>([&]{(void)PersistentMainGraph::open(path,identity,memory,1,policy,missing);});
  Resolver wrong;wrong.sources={{id(1),&b},{id(2),&b},{id(3),&c}};
  throws<std::runtime_error>([&]{(void)PersistentMainGraph::open(path,identity,memory,1,policy,wrong);});
  // Forge a checksum-valid first merge with a wrong core-result digest.
  auto block=ExperienceBlock::open_reader(path/"m-0000000000000000.block");
  auto stored=block.read(first_head,1024,memory);auto view=stored.view();std::vector<std::byte> data(view.content.begin(),view.content.end());data[144]^=std::byte{1};view.content=data;
  fs::rename(path/"m-0000000000000000.block",path/"retained-original.block");
  {
   auto forged=ExperienceBlock::create(path/"m-0000000000000000.block",block.identity(),block.capacity());(void)forged.append(view);
  }
  bool core_mismatch=false;
  try{(void)PersistentMainGraph::open(path,identity,memory,1,policy,resolver);}
  catch(const std::runtime_error& error){core_mismatch=std::string_view(error.what())=="stored Main merge disagrees with SWEGCA replay";}
  CHECK(core_mismatch);
 }
 CHECK(memory.used()==0);fs::remove_all(root);std::printf("persistent Main tests: %u checks passed\n",checks);
}
