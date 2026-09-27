#include "vrs/portal_page.hpp"
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <limits>
#include <unistd.h>
using namespace swegca::architecture;
using namespace swegca::architecture::kernel;
using namespace swegca::vrs;
static unsigned checks=0;
#define CHECK(x) do{++checks;if(!(x)){std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);std::abort();}}while(false)
template<class E,class F>void rejects(F fn){bool caught=false;try{fn();}catch(const E&){caught=true;}CHECK(caught);}
static long remaining=-1;
extern "C" ssize_t __real_pwrite(int,const void*,size_t,off_t);
extern "C" ssize_t __wrap_pwrite(int fd,const void* data,size_t n,off_t at){
 if(at>=static_cast<off_t>(ExperienceBlock::header_bytes)&&remaining>=0){
  if(!remaining){errno=EIO;return -1;}
  n=std::min(n,static_cast<size_t>(remaining));
  auto result=__real_pwrite(fd,data,n,at);if(result>0)remaining-=result;return result;
 }
 return __real_pwrite(fd,data,n,at);
}
DigestBytes id(unsigned n){DigestBytes d{};d[0]=std::byte(n);return d;}
int main(){
 auto text=(std::filesystem::temp_directory_path()/"swegca-portals-XXXXXX").string();CHECK(::mkdtemp(text.data()));
 const std::filesystem::path root(text);MemoryBudget memory(1<<20);StorageBudget storage(1<<20);
 std::array<PortalRange,256> ranges{};
 for(unsigned n=0;n<ranges.size();++n)ranges[n]={id(3),3*n,3*n+1};
 for(const auto kind:{PortalPage::Kind::cue,PortalPage::Kind::context}){
  {
   auto page=PortalPage::create(root/"page",id(1),kind,id(2),ranges,memory,&storage);
   CHECK(page.size()==256&&page.kind()==kind&&page.lookup()==id(2));
   CHECK(memory.used()==0&&storage.used()==std::filesystem::file_size(root/"page"));
   const auto charged=storage.used();
   {auto loaded=page.load(memory);CHECK(loaded.size()==ranges.size());
    for(unsigned n=0;n<ranges.size();++n)CHECK(loaded[n]==ranges[n]);}
   CHECK(memory.used()==0);
   MemoryBudget tiny(1);rejects<std::bad_alloc>([&]{(void)page.load(tiny);});CHECK(tiny.used()==0&&storage.used()==charged);
   auto moved=std::move(page);CHECK(moved.load(memory).front()==ranges.front());
  }
  CHECK(!std::filesystem::exists(root/"page")&&storage.used()==0&&memory.used()==0);
 }
 auto invalid=[&](std::span<const PortalRange> values){
  rejects<std::invalid_argument>([&]{(void)PortalPage::create(root/"invalid",id(1),PortalPage::Kind::cue,id(2),values,memory,&storage);});
  CHECK(!std::filesystem::exists(root/"invalid")&&memory.used()==0&&storage.used()==0);
 };
 invalid({});std::array<PortalRange,257> excess{};invalid(excess);
 for(auto bad:{PortalRange{{},0,1},PortalRange{id(3),1,1},PortalRange{id(3),2,1}})invalid(std::span(&bad,1));
 auto unordered=std::array{PortalRange{id(3),0,4},PortalRange{id(3),3,5}};invalid(unordered);
 unordered={PortalRange{id(4),0,1},PortalRange{id(3),2,3}};invalid(unordered);
 rejects<std::invalid_argument>([&]{(void)PortalPage::create(root/"invalid",id(1),static_cast<PortalPage::Kind>(0),id(2),ranges,memory,&storage);});
 rejects<std::invalid_argument>([&]{(void)PortalPage::create(root/"invalid",id(1),PortalPage::Kind::cue,{},ranges,memory,&storage);});
 // Adjacent nonoverlapping ranges and the full uint64 index boundary survive.
 {
  std::array boundary{PortalRange{id(3),0,1},PortalRange{id(3),1,2},
    PortalRange{id(4),UINT64_MAX-1,UINT64_MAX}};
  auto page=PortalPage::create(root/"boundary",id(5),PortalPage::Kind::context,id(6),boundary,memory,&storage);
  auto loaded=page.load(memory);CHECK(loaded.back()==boundary.back());
 }
 CHECK(storage.used()==0);
 // Partial body failures discard only the derived owned inode and reservation.
 for(const auto partial:{0,13}){
  remaining=partial;rejects<std::system_error>([&]{(void)PortalPage::create(root/"failed",id(7),PortalPage::Kind::cue,id(2),ranges,memory,&storage);});
  remaining=-1;CHECK(storage.used()==0&&memory.used()==0&&!std::filesystem::exists(root/"failed"));
 }
 {
  StorageBudget tiny(ExperienceBlock::header_bytes);
  rejects<StorageLimit>([&]{(void)PortalPage::create(root/"quota",id(8),PortalPage::Kind::cue,id(2),ranges,memory,&tiny);});
  CHECK(tiny.used()==0&&!std::filesystem::exists(root/"quota"));
 }
 {
  auto page=PortalPage::create(root/"corrupt",id(9),PortalPage::Kind::cue,id(2),ranges,memory,&storage);
  const int fd=::open((root/"corrupt").c_str(),O_WRONLY);CHECK(fd>=0);char changed=127;
  CHECK(::pwrite(fd,&changed,1,ExperienceBlock::header_bytes+ExperienceBlock::record_overhead+100)==1);::close(fd);
  rejects<std::runtime_error>([&]{(void)page.load(memory);});CHECK(memory.used()==0);
 }
 CHECK(storage.used()==0&&!std::filesystem::exists(root/"corrupt"));
 {
  auto page=PortalPage::create(root/"linked",id(10),PortalPage::Kind::cue,id(2),ranges,memory,&storage);
  CHECK(::link((root/"linked").c_str(),(root/"alias").c_str())==0);
 }
 CHECK(storage.used()!=0&&std::filesystem::exists(root/"linked")&&std::filesystem::exists(root/"alias"));
 {
  const auto existing=root/"existing";const int fd=::open(existing.c_str(),O_CREAT|O_WRONLY|O_EXCL,0600);CHECK(fd>=0);
  const char original[]="not a derived page";CHECK(::write(fd,original,sizeof(original))==sizeof(original));::close(fd);
  const auto before=storage.used();
  rejects<std::system_error>([&]{(void)PortalPage::create(existing,id(11),PortalPage::Kind::cue,id(2),ranges,memory,&storage);});
  CHECK(std::filesystem::file_size(existing)==sizeof(original)&&storage.used()==before);
  {
   auto page=PortalPage::create(root/"replace",id(12),PortalPage::Kind::cue,id(2),ranges,memory,&storage);
   std::filesystem::rename(root/"replace",root/"owned-renamed");
   std::filesystem::copy_file(existing,root/"replace");
  }
  CHECK(std::filesystem::exists(root/"owned-renamed")&&std::filesystem::file_size(root/"replace")==sizeof(original));
 }
 CHECK(memory.used()==0);std::filesystem::remove_all(root);
 std::printf("portal page tests: %u checks passed\n",checks);
}
