#include "vrs/experience_page.hpp"
#include "vrs/experience_sequence.hpp"
#include <thread>
#include "swegca_architecture/evidence_rules.hpp"
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>
using namespace swegca::vrs;
using namespace swegca::architecture;
using namespace swegca::architecture::kernel;
static std::int64_t body_write_remaining=-1,header_write_remaining=-1;
static bool fail_header_sync=false,fail_directory_sync=false;
static bool fail_body_sync=false,body_written=false;
extern "C" ssize_t __real_pwrite(int,const void*,size_t,off_t);
extern "C" int __real_fdatasync(int);
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd){if(fail_directory_sync){errno=EIO;return -1;}return __real_fsync(fd);}
extern "C" ssize_t __wrap_pwrite(int fd,const void* bytes,size_t count,off_t offset){
 if(offset>=static_cast<off_t>(ExperienceBlock::header_bytes)){
  if(body_write_remaining==0){errno=EIO;return -1;}
  if(body_write_remaining>0)count=std::min(count,static_cast<size_t>(body_write_remaining));
  const auto result=__real_pwrite(fd,bytes,count,offset);
  if(result>0){body_written=true;if(body_write_remaining>0)body_write_remaining-=result;}
  return result;
 }
 if(header_write_remaining==0){errno=EIO;return -1;}
 if(header_write_remaining>0)count=std::min(count,static_cast<size_t>(header_write_remaining));
 const auto result=__real_pwrite(fd,bytes,count,offset);
 if(result>0&&header_write_remaining>0)header_write_remaining-=result;
 return result;
}
extern "C" int __wrap_fdatasync(int fd){
 if(fail_header_sync||(fail_body_sync&&body_written)){errno=EIO;return -1;}
 return __real_fdatasync(fd);
}
static std::atomic<unsigned> checks=0;
#define CHECK(x) do{++checks;if(!(x)){std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);std::abort();}}while(false)
template<class E,class F>void rejects(F f){bool caught=false;try{f();}catch(const E&){caught=true;}CHECK(caught);}
DigestBytes id(unsigned n){DigestBytes value{};value[0]=std::byte(n);return value;}
int main(){
 auto path=(std::filesystem::temp_directory_path()/"swegca-pages-XXXXXX").string();CHECK(::mkdtemp(path.data()));
 const auto root=std::filesystem::path(path);
 MemoryBudget memory(4<<20);EvidencePolicy policy;policy.axis_count=2;const auto rules=make_evidence_rules(policy);
 StorageBudget storage(2<<20);
 {
  auto original=ExperienceBlock::create(root/"original",id(1),1<<20,&storage);
  std::pmr::vector<ExperienceEvidence> values(&memory);
  for(unsigned n=0;n<256;++n){
   EvidenceObservation value;value.hypothesis=id(2);value.source=id(3);value.context=id(4);value.producer=id(5);
   value.observed_at=n;value.expires_at=n+100;value.has_expiry=n%2;value.axis=n%2;
   value.producer_confidence=n/255.;value.outcome=static_cast<EvidenceOutcome>(n%3);
   values.push_back(record_evidence(original,rules,{n,n,"session","test","text/plain",{}},value,n%2?std::optional(id(6)):std::nullopt));
  }
  {
   // A page header fits but its record does not. Every retry must release
   // its private header and quota; originals and their owning budget survive.
   StorageBudget small(ExperienceBlock::header_bytes);
   const auto original_bytes=storage.used();
   for(unsigned attempt=0;attempt<3;++attempt){
    const auto destination=root/("quota-page-"+std::to_string(attempt));
    rejects<StorageLimit>([&]{(void)ExperiencePage::create(destination,id(70),values,memory,&small);});
    CHECK(small.used()==0&&!std::filesystem::exists(destination));
    CHECK(storage.used()==original_bytes&&std::filesystem::exists(root/"original"));
   }
  }
  {
   const auto before=storage.used();
   for(const std::int64_t allowed:{0,1,17,128,4096}){
    body_write_remaining=allowed;
    rejects<std::system_error>([&]{(void)ExperiencePage::create(root/"failed-write",id(71),values,memory,&storage);});
    body_write_remaining=-1;
    CHECK(storage.used()==before&&!std::filesystem::exists(root/"failed-write"));
   }
   body_written=false;fail_body_sync=true;
   rejects<std::system_error>([&]{(void)ExperiencePage::create(root/"failed-sync",id(72),values,memory,&storage);});
   fail_body_sync=false;
   CHECK(storage.used()==before&&!std::filesystem::exists(root/"failed-sync"));
   CHECK(std::filesystem::exists(root/"original"));
  }
  {
   const auto before=storage.used();
   for(const std::int64_t allowed:{0,1,17,79}){
    header_write_remaining=allowed;
    rejects<std::system_error>([&]{(void)ExperiencePage::create(root/"failed-header",id(73),values,memory,&storage);});
    header_write_remaining=-1;
    CHECK(storage.used()==before&&!std::filesystem::exists(root/"failed-header"));
   }
   for(bool directory:{false,true}){
    fail_header_sync=!directory;fail_directory_sync=directory;
    rejects<std::system_error>([&]{(void)ExperiencePage::create(root/"failed-header-sync",id(74),values,memory,&storage);});
    fail_header_sync=false;fail_directory_sync=false;
    CHECK(storage.used()==before&&!std::filesystem::exists(root/"failed-header-sync"));
   }
   // Original creation keeps its pre-existing conservative recovery contract.
   StorageBudget original_budget(4096);header_write_remaining=1;
   rejects<std::system_error>([&]{(void)ExperienceBlock::create(root/"original-failed-header",id(75),4096,&original_budget);});
   header_write_remaining=-1;
   CHECK(std::filesystem::file_size(root/"original-failed-header")==1);
   CHECK(original_budget.used()==ExperienceBlock::header_bytes);
  }
  {
   for(unsigned owners=0;owners<4;++owners)for(bool complete:{false,true})for(bool backed:{false,true}){
    const auto decision=metadata_release(complete,owners,backed);
    CHECK(decision==(!complete||owners!=1?MetadataRelease::retain:
        backed?MetadataRelease::release:MetadataRelease::persist_then_release));
   }
   ExperienceSequence sequence(memory);
   for(const auto& value:values){sequence.prepare_append();sequence.commit_append(value);}
   // Index 127 belongs to the full 128-value segment; index 255 is an active tail.
   CHECK(!sequence.page_out(255,root/"tail",id(20),rules,&storage));
   {
    auto pin=sequence.pin(127);
    CHECK(!sequence.page_out(127,root/"pinned",id(21),rules,&storage));
    CHECK(pin->original()==values[127].original());
   }
   const auto before=memory.used();
   CHECK(sequence.page_out(127,root/"segment",id(22),rules,&storage));
   const auto cold=memory.used();CHECK(cold<before);
   CHECK(!sequence.page_out(127,root/"unused",id(23),rules,&storage));
   const auto held_bytes=memory.limit()-memory.used();auto* held=memory.allocate(held_bytes);
   rejects<std::bad_alloc>([&]{(void)sequence[127];});
   memory.deallocate(held,held_bytes);CHECK(memory.used()==cold);
   const auto segment_bytes=128*sizeof(ExperienceEvidence);
   // Output allocation succeeds but authenticated input allocation fails:
   // the unpublished segment must be released and remain retryable.
   const auto held_output=memory.limit()-memory.used()-segment_bytes;
   auto* output_hold=memory.allocate(held_output);
   rejects<std::bad_alloc>([&]{(void)sequence[127];});
   memory.deallocate(output_hold,held_output);CHECK(memory.used()==cold);
   // Enough for one final segment and its encoded record, but not two decoded
   // arrays. This rejects the former load-vector + copy implementation.
   const auto restore_budget=segment_bytes+std::filesystem::file_size(root/"segment")-ExperienceBlock::header_bytes;
   CHECK(restore_budget<2*segment_bytes);
   const auto held_restore=memory.limit()-memory.used()-restore_budget;
   auto* restore_hold=memory.allocate(held_restore);
   CHECK(sequence[254].original()==values[254].original());
   memory.deallocate(restore_hold,held_restore);
   CHECK(memory.used()==cold+segment_bytes);
   CHECK(sequence.page_out(127,root/"unused",id(23),rules,&storage));
   // Readers may restore the same shared segment concurrently; publish once.
   std::thread first([&]{CHECK(sequence[127].original()==values[127].original());});
   std::thread second([&]{CHECK(sequence[254].original()==values[254].original());});
   first.join();second.join();
   CHECK(memory.used()>cold);
   const auto written=storage.used();
   CHECK(sequence.page_out(127,root/"unused",id(23),rules,&storage));
   CHECK(storage.used()==written&&memory.used()==cold);
   {
    auto snapshot=sequence.snapshot(memory,127,255);
    CHECK(!sequence.page_out(127,root/"unused",id(23),rules,&storage));
    CHECK(snapshot[0].original()==values[127].original());
    CHECK(snapshot[127].original()==values[254].original());
    CHECK(!sequence.page_out(127,root/"unused",id(23),rules,&storage));
   }
   CHECK(sequence.page_out(127,root/"unused",id(23),rules,&storage));
   ExperienceSequence copy(memory);copy.share_prefix(sequence);
   CHECK(!sequence.page_out(127,root/"unused",id(23),rules,&storage));
   CHECK(copy[127].original()==values[127].original());
   CHECK(sequence[127].original()==values[127].original());
   CHECK(!std::filesystem::exists(root/"tail")&&!std::filesystem::exists(root/"pinned")&&!std::filesystem::exists(root/"unused"));
  }
  {
   ExperienceSequence sequence(memory);
   for(const auto& value:values){sequence.prepare_append();sequence.commit_append(value);}
   CHECK(sequence.page_out(63,root/"read-64",id(40),rules,&storage));
   CHECK(sequence.page_out(127,root/"read-128",id(41),rules,&storage));
   const auto cold=memory.used();
   {
    auto reader=sequence.reader();
    for(unsigned n=0;n<12;++n){
     const auto index=n%2?63:127;
     CHECK(reader[index].original()==values[index].original());
     CHECK(memory.used()<=cold+128*sizeof(ExperienceEvidence));
    }
   }
   CHECK(memory.used()==cold);
   {
    auto snapshot=sequence.snapshot(memory,65,200);
    const auto pinned=memory.used();
    {
     ExperienceSequence::Snapshot::Reader reader(snapshot,memory);
     for(unsigned n=0;n<12;++n){
      const auto index=n%2?0:100;
      CHECK(reader[index].original()==values[65+index].original());
      CHECK(memory.used()<=pinned+128*sizeof(ExperienceEvidence));
     }
     rejects<std::out_of_range>([&]{(void)reader[135];});
    }
    CHECK(memory.used()==pinned);
   }
   CHECK(memory.used()==cold);
   CHECK(!sequence.page_out(63,root/"unused",id(42),rules,&storage));
   CHECK(!sequence.page_out(127,root/"unused",id(43),rules,&storage));
  }
  CHECK(!std::filesystem::exists(root/"segment"));
  {
   alignas(StorageBudget) std::byte slot[sizeof(StorageBudget)];
   auto* budget=std::construct_at(reinterpret_cast<StorageBudget*>(slot),1<<20);
   std::optional<ExperiencePage> page;
   page.emplace(ExperiencePage::create(root/"late-release",id(44),values,memory,budget));
   // A retained snapshot can release its page after its original Runtime dies.
   // Reuse the exact budget address to detect charging an unrelated new owner.
   std::destroy_at(budget);
   budget=std::construct_at(reinterpret_cast<StorageBudget*>(slot),1<<20,100000);
   page.reset();
   CHECK(budget->used()==100000);
   CHECK(!std::filesystem::exists(root/"late-release"));
   std::destroy_at(budget);
  }
  CHECK(!metadata_pressure(9,10,false)&&!metadata_pressure(10,10,false));
  CHECK(metadata_pressure(11,10,false)&&!metadata_pressure(11,10,true));
  CHECK(!metadata_page_beneficial(false,10,10)&&metadata_page_beneficial(false,11,10));
  CHECK(metadata_page_beneficial(true,1,10));
  for(bool same:{false,true})for(bool sole:{false,true})CHECK(discard_metadata_page(same,sole)==(same&&sole));
  {
   const auto baseline=storage.used();
   {
    auto a=ExperiencePage::create(root/"owned-a",id(30),values,memory,&storage);
    auto b=ExperiencePage::create(root/"owned-b",id(31),values,memory,&storage);
    const auto bytes=std::filesystem::file_size(root/"owned-a");
    CHECK(storage.used()==baseline+2*bytes);
    a=std::move(b);CHECK(!std::filesystem::exists(root/"owned-a"));
    CHECK(storage.used()==baseline+bytes);
    CHECK(a.read(0,rules,memory).original()==values[0].original());
   }
   CHECK(!std::filesystem::exists(root/"owned-b")&&storage.used()==baseline);
  }
  {
   auto page=ExperiencePage::create(root/"shared",id(32),values,memory,&storage);
   std::filesystem::create_hard_link(root/"shared",root/"alias");
  }
  CHECK(std::filesystem::exists(root/"shared")&&std::filesystem::exists(root/"alias"));
  {
   auto page=ExperiencePage::create(root/"replaced",id(33),values,memory,&storage);
   std::filesystem::rename(root/"replaced",root/"renamed");
   std::filesystem::create_symlink(root/"original",root/"replaced");
  }
  CHECK(std::filesystem::is_symlink(root/"replaced")&&std::filesystem::exists(root/"renamed"));
  CHECK(std::filesystem::exists(root/"original"));
  rejects<std::invalid_argument>([&]{(void)ExperiencePage::create(root/"empty",id(7),{},memory);});
  CHECK(!std::filesystem::exists(root/"empty"));
  auto page=ExperiencePage::create(root/"page",id(8),values,memory,&storage);CHECK(page.size()==256);
  MemoryBudget read_memory(80000);
  for(unsigned n=0;n<256;++n){
   const auto restored=page.read(n,rules,read_memory);const auto& expected=values[n];
   CHECK(restored.original()==expected.original()&&restored.cue()==expected.cue()&&restored.has_input_key()==expected.has_input_key());
   const auto& a=restored.value();const auto& b=expected.value();
   CHECK(a.hypothesis==b.hypothesis&&a.address==b.address&&a.source==b.source&&a.context==b.context&&a.producer==b.producer);
   CHECK(a.observed_at==b.observed_at&&a.expires_at==b.expires_at&&a.has_expiry==b.has_expiry);
   CHECK(a.axis==b.axis&&a.outcome==b.outcome&&a.producer_confidence==b.producer_confidence);
   CHECK(read_memory.used()==0);
  }
  {
   MemoryBudget page_memory(200000);auto loaded=page.load(rules,page_memory);
   CHECK(loaded.size()==values.size());
   for(unsigned n=0;n<256;++n)CHECK(loaded[n].original()==values[n].original()&&loaded[n].cue()==values[n].cue());
  }
  rejects<std::out_of_range>([&]{(void)page.read(256,rules,read_memory);});
  MemoryBudget tiny(1);rejects<std::bad_alloc>([&]{(void)page.read(0,rules,tiny);});CHECK(tiny.used()==0);
  auto changed=policy;changed.axis_count=1;const auto narrow=make_evidence_rules(changed);
  rejects<std::runtime_error>([&]{(void)page.read(1,narrow,read_memory);});
  // Corrupt another entry: selecting entry zero must still authenticate the full page.
  const auto size=std::filesystem::file_size(root/"page");const auto fd=::open((root/"page").c_str(),O_RDWR);
  CHECK(fd>=0);char byte=0;CHECK(::pread(fd,&byte,1,size-64)==1);const char altered=byte^1;
  CHECK(::pwrite(fd,&altered,1,size-64)==1);
  rejects<std::runtime_error>([&]{(void)page.read(0,rules,read_memory);});CHECK(read_memory.used()==0);
  MemoryBudget page_memory(200000);
  rejects<std::runtime_error>([&]{(void)page.load(rules,page_memory);});CHECK(page_memory.used()==0);
  CHECK(::pwrite(fd,&byte,1,size-64)==1);::close(fd);
  auto moved=std::move(page);CHECK(moved.read(255,rules,read_memory).original()==values.back().original());
  rejects<std::exception>([&]{(void)page.read(0,rules,read_memory);});
  CHECK(decode_evidence(rules,original.read(values[0].original(),4096,memory)).original()==values[0].original());
 }
 CHECK(memory.used()==0);std::filesystem::remove_all(root);
 std::printf("experience page tests: %u checks passed\n",checks.load());
}
