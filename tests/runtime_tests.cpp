#include "vrs/runtime.hpp"
#include "vrs/storage_inventory.hpp"
#include "swegca_architecture/input_cue.hpp"
#include "swegca_architecture/agent_delivery_identity.hpp"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
using namespace swegca::architecture;
using namespace swegca::architecture::kernel;
using namespace swegca::vrs;
namespace fs=std::filesystem;
static unsigned checks=0,reads=0,writes=0;static bool fail_write=false,fail_read=false;
#define CHECK(e) do{++checks;if(!(e)){std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#e);std::abort();}}while(false)
template<class E,class F>void throws(F f){bool caught=false;try{f();}catch(const E&){caught=true;}CHECK(caught);}
extern "C" ssize_t __real_pread(int,void*,size_t,off_t);
extern "C" ssize_t __wrap_pread(int fd,void* p,size_t n,off_t o){++reads;if(fail_read){fail_read=false;errno=EIO;return -1;}return __real_pread(fd,p,n,o);}
extern "C" ssize_t __real_pwrite(int,const void*,size_t,off_t);
extern "C" ssize_t __wrap_pwrite(int fd,const void* p,size_t n,off_t o){++writes;if(fail_write){fail_write=false;errno=ENOSPC;return -1;}return __real_pwrite(fd,p,n,o);}
class FailingMemory final:public std::pmr::memory_resource {
public: bool fail=false;
private:
 void* do_allocate(std::size_t n,std::size_t a) override {if(fail)throw std::bad_alloc();return std::pmr::new_delete_resource()->allocate(n,a);}
 void do_deallocate(void* p,std::size_t n,std::size_t a) override {std::pmr::new_delete_resource()->deallocate(p,n,a);}
 bool do_is_equal(const std::pmr::memory_resource& other)const noexcept override{return this==&other;}
};
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
  const auto transfers=host.storage().transfer().requested();
  const auto r=reads,w=writes;auto recalled=host.input("text/plain",content);
  CHECK(recalled.temporary()&&reads==r&&writes==w);
  CHECK(host.storage().transfer().requested()==transfers);
  {
   auto slice=host.read_payload_slice(recalled,0,2,5);
   CHECK(slice.evidence().original()==first && slice.total_bytes()==content.size());
   CHECK(std::ranges::equal(slice.content(),content.subspan(2,5)));
  }
  CHECK(host.replay(recalled,0).location()==first);
  CHECK(host.storage().transfer().requested()>transfers);
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
  {
   auto slice=host.read_payload_slice(recalled,0,2,5);
   CHECK(slice.evidence().original()==first && slice.total_bytes()==content.size());
   CHECK(std::ranges::equal(slice.content(),content.subspan(2,5)));
  }
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
  CHECK(host.compare_replay(host.replay(recalled,0),9,0).agreement()==ReplayAgreement::insufficient);
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
 CHECK(memory.used()==0);
 {
  const auto path=root/"receive";fs::create_directory(path);
  auto host=Runtime::create(path,config,memory);host.start_session(id(10),"events");
  ExperienceLocation initial;
  unsigned sequence=0;
  for(std::string_view source:{"user","assistant","tool","system"}){
   auto event=host.receive({sequence,0,"events",source,"text/plain",content},7,0);
   CHECK(event.recalled.matches().size()==sequence);
   CHECK(event.recorded.refinement.result().verification().judgment().status()==EvidenceStatus::abstain);
   if(sequence==0){CHECK(!event.recalled.familiar());initial=event.recorded.original;
    const auto r=reads,w=writes;CHECK(!host.cognize(event.recalled,7,0));CHECK(reads==r&&writes==w);
   }
   else {
    // Receive has already appended the new event to the same connection.
    // The receipt still names only experience that existed before that append.
    CHECK(event.recalled.temporary());
    CHECK(event.recalled.matches()[0].original==initial);
    CHECK(event.recalled.matches()[0].recalled.recalled_head.observations==sequence);
    const auto w=writes;
    fail_read=true;throws<std::system_error>([&]{(void)host.cognize(event.recalled,7,0);});
    CHECK(writes==w);
    auto cognition=host.cognize(event.recalled,7,0);
    CHECK(cognition&&cognition->replayed.location()==initial&&!cognition->reverified);
    CHECK(cognition->assessment().agreement()==ReplayAgreement::insufficient);
    CHECK(cognition->assessment().current_originals().size()==1);
    CHECK(cognition->assessment().current_originals()[0]==event.recorded.original);
    CHECK(writes==w);
    auto replayed=host.replay(event.recalled,0);CHECK(replayed.location()==initial);
    auto compared=host.compare_replay(replayed,7,0);
    const auto& checked=compared.evidence();
    CHECK(checked.agreement()==ReplayAgreement::insufficient);
    throws<std::invalid_argument>([&]{(void)host.re_evidence(replayed,compared,7,0);});
    CHECK(checked.current_originals().size()==1&&checked.current_originals()[0]==event.recorded.original);
   }
   auto current=host.input("text/plain",content);CHECK(current.matches().size()==sequence+1);
   auto retained=host.replay(current,sequence);auto original=evidence_payload(retained.original());
   CHECK(original.source==source&&std::ranges::equal(original.content,content));
   ++sequence;
  }
  const std::array<std::byte,5> binary{std::byte{0},std::byte{255},std::byte{10},std::byte{0},std::byte{127}};
  auto tool=host.receive({4,0,"events","tool","application/octet-stream",binary},7,0);
  CHECK(tool.recorded.original.bytes>binary.size());
  auto stored=host.replay(host.input("application/octet-stream",binary),0);
  CHECK(std::ranges::equal(evidence_payload(stored.original()).content,binary));
  CHECK(host.main().graph().generation()==0);host.end_session();CHECK(host.work(7,0)==1);
  host.start_session(id(11),"next");
  auto event=host.receive({0,0,"next","user","text/plain",content},7,0);
  CHECK(!event.recalled.temporary()&&event.recalled.matches().size()==4);
  CHECK(host.replay(event.recalled,0).location()==initial);
  CHECK(host.input("text/plain",content).temporary());
  fail_write=true;
  throws<std::system_error>([&]{(void)host.receive({1,0,"next","assistant","text/plain",content},7,0);});
  CHECK(!host.session().usable());
 }
 {
  const auto budget_root=root/"budget-runtime";fs::create_directory(budget_root);
  std::uint64_t initial=0,charged=0;
  {
   auto host=Runtime::create(budget_root,config,memory);
   throws<std::system_error>([&]{(void)Runtime::open(budget_root,config,memory);});
   initial=stored_bytes(budget_root,memory);CHECK(host.storage().used()==initial);
   host.start_session(id(80),"quota");
   for(unsigned n=0;n<5;++n){
    (void)host.retain({n,0,"quota","user","text/plain",content},7,0);
    CHECK(host.storage().used()==stored_bytes(budget_root,memory));
   }
   host.end_session();CHECK(host.work(7,0)==1);
   charged=host.storage().used();CHECK(charged==stored_bytes(budget_root,memory));
  }
  const auto actual=stored_bytes(budget_root,memory);CHECK(actual>initial);
  // Publication aliases are hard links: adding another name costs no payload.
  fs::create_hard_link(budget_root/"graph"/"control.block",budget_root/"alias.block");
  CHECK(stored_bytes(budget_root,memory)==actual);
  auto exact=config;exact.storage_bytes=actual;
  {
   auto host=Runtime::open(budget_root,exact,memory);
   CHECK(host.storage().used()==actual);
   const auto before=writes;
   throws<StorageLimit>([&]{host.start_session(id(81),"denied");});
   CHECK(writes==before&&host.storage().used()==actual);
   CHECK(stored_bytes(budget_root,memory)==actual);
  }
  exact.storage_bytes=actual-1;
  throws<StorageLimit>([&]{(void)Runtime::open(budget_root,exact,memory);});
  CHECK(stored_bytes(budget_root,memory)==actual);
  fs::create_symlink(budget_root/"graph"/"control.block",budget_root/"unsafe-link");
  throws<std::runtime_error>([&]{(void)stored_bytes(budget_root,memory);});
  fs::remove(budget_root/"unsafe-link");
 }
 CHECK(memory.used()==0);
 {
  const auto path=root/"expired-owner";fs::create_directory(path);
  auto host=Runtime::create(path,config,memory);host.start_session(id(90),"old");
  const auto retained=host.retain({0,0,"old","user","text/plain",content},7,0).original;
  auto receipt=host.input("text/plain",content);auto replayed=host.replay(receipt,0);
  CHECK(replayed.location()==retained);
  host.end_session();host.start_session(id(91),"new");
  const auto r=reads,w=writes;
  throws<std::invalid_argument>([&]{(void)host.replay(receipt,0);});
  throws<std::invalid_argument>([&]{(void)host.read_payload_slice(receipt,0,0,1);});
  throws<std::invalid_argument>([&]{(void)host.compare_replay(replayed,7,0);});
  CHECK(reads==r&&writes==w);
  CHECK(receipt.matches()[0].original==retained);
  host.end_session();
 }
 CHECK(memory.used()==0);
 {
  const auto path=root/"warm-handoff";fs::create_directory(path);
  auto host=Runtime::create(path,config,memory);host.start_session(id(92),"ended");
  ExperienceLocation original;
  for(unsigned n=0;n<32;++n){auto saved=host.retain({n,0,"ended","user","text/plain",content},7,0);if(!n)original=saved.original;}
  host.end_session();CHECK(!host.has_session()&&host.main().graph().generation()==0);
  host.start_session(id(93),"active");const auto* active=&host.session();
  const std::string other="separate active input";const auto other_bytes=std::as_bytes(std::span(other));
  const auto fresh=host.retain({0,0,"active","user","text/plain",other_bytes},7,0).original;
  const auto before=reads;
  CHECK(host.work(7,0)==1);
  CHECK(reads==before); // The ended source was already verified; no history re-read.
  CHECK(&host.session()==active&&host.session().phase()==SessionPhase::active);
  auto recalled=host.input("text/plain",content);CHECK(!recalled.temporary()&&recalled.matches().size()==32);
  CHECK(host.replay(recalled,0).location()==original);
  auto local=host.input("text/plain",other_bytes);CHECK(local.temporary());
  CHECK(host.replay(local,0).location()==fresh);
  (void)host.retain({1,0,"active","assistant","text/plain",other_bytes},7,0);
  CHECK(host.work(7,0)==0);host.end_session();
  const auto next_reads=reads;CHECK(host.work(7,0)==1);CHECK(reads==next_reads);
 }
 CHECK(memory.used()==0);
 {
  FailingMemory upstream;MemoryBudget bounded(64<<20,&upstream);
  const auto path=root/"handoff-allocation-failure";fs::create_directory(path);
  {
   auto host=Runtime::create(path,config,bounded);host.start_session(id(94),"closed");
   const auto original=host.retain({0,0,"closed","user","text/plain",content},7,0).original;
   host.end_session();host.start_session(id(95),"still-active");
   const std::string other="active after failed work";const auto bytes=std::as_bytes(std::span(other));
   const auto local=host.retain({0,0,"still-active","user","text/plain",bytes},7,0).original;
   upstream.fail=true;throws<std::bad_alloc>([&]{(void)host.work(7,0);});upstream.fail=false;
   CHECK(host.main().usable()&&host.main().graph().generation()==0&&host.has_session());
   CHECK(host.replay(host.input("text/plain",bytes),0).location()==local);
   const auto before=reads;CHECK(host.work(7,0)==1);CHECK(reads>before);
   CHECK(host.main().graph().source_count()==1);
   CHECK(host.main().graph().replay(input_cue("text/plain",content),0).location()==original);
  }
  CHECK(bounded.used()==0);
 }
 {
  const auto path=root/"bounded-payload";fs::create_directory(path);
  auto large=config;large.session_block_capacity=8<<20;large.read_limit=8<<20;
  MemoryBudget bounded(256<<10);
  {
   auto host=Runtime::create(path,large,bounded);host.start_session(id(96),"large");
   std::vector<std::byte> payload(2<<20);
   for(std::size_t n=0;n<payload.size();++n)payload[n]=std::byte(n%251);
   const auto original=host.retain({0,0,"large","tool","application/octet-stream",payload},7,0).original;
   auto receipt=host.input("application/octet-stream",payload);
   const auto check_part=[&](const InputRecall& recalled){
    const auto before_writes=writes;const auto used=bounded.used();
    {
     auto part=host.read_payload_slice(recalled,0,65530,4096);
     CHECK(part.evidence().original()==original && part.total_bytes()==payload.size());
     CHECK(std::ranges::equal(part.content(),std::span(payload).subspan(65530,4096)));
     CHECK(bounded.used()-used==4096);
    }
    CHECK(bounded.used()==used && writes==before_writes);
    throws<std::out_of_range>([&]{(void)host.read_payload_slice(recalled,1,0,1);});
    throws<std::invalid_argument>([&]{(void)host.read_payload_slice(recalled,0,payload.size(),1);});
    throws<std::bad_alloc>([&]{(void)host.replay(recalled,0);});
    CHECK(!host.input("unseen/media",{}).familiar()); // partial read is not a continuation
   };
   CHECK(receipt.temporary());check_part(receipt);
   host.end_session();CHECK(host.work(7,0)==1);host.start_session(id(97),"reader");
   auto recalled=host.input("application/octet-stream",payload);
   CHECK(!recalled.temporary());check_part(recalled);
   throws<std::invalid_argument>([&]{(void)host.read_payload_slice(receipt,0,0,1);});
   CHECK(host.main().graph().generation()==1);
  }
  CHECK(bounded.used()==0);
 }
 {
  const auto path=root/"native-envelope";fs::create_directory(path);
  auto cfg=config;cfg.session_block_capacity=1<<20;cfg.read_limit=1<<20;
  const std::string prompt="사용자 입력";
  const auto cue=std::as_bytes(std::span(prompt));
  const std::string envelope="{native bytes, metadata, unknown fields:"+std::string(70000,'x')+"}";
  const auto raw=std::as_bytes(std::span(envelope));
  ExperienceLocation saved;
  {
   auto host=Runtime::create(path,cfg,memory);host.start_session(id(91),"native");
   auto first=host.receive_envelope("text/plain",cue,{0,0,"native","agent","application/json",raw},7,0);
   saved=first.recorded.original;
   CHECK(!first.recalled.familiar());
   auto recall=host.input("text/plain",cue);
   CHECK(recall.temporary() && recall.matches().size()==1);
   auto envelope_context=host.input("application/json",raw);
   CHECK(envelope_context.key_kind()==FamiliarityKey::context);
   CHECK(envelope_context.matches().size()==1&&envelope_context.matches()[0].original==saved);
   auto part=host.read_payload_slice(recall,0,65520,48);
   CHECK(part.evidence().cue()==input_cue("text/plain",cue));
   CHECK(part.total_bytes()==raw.size() && std::ranges::equal(part.content(),raw.subspan(65520,48)));
   auto replay=host.replay(recall,0);auto original=evidence_payload(replay.original());
   CHECK(replay.location()==saved && original.media_type=="application/json");
   CHECK(std::ranges::equal(original.content,raw));
   CHECK(host.work(7,0)==0);
  }
  CHECK(memory.used()==0);
  {
   auto host=Runtime::open(path,cfg,memory);host.resume_session(id(91));
   auto second=host.receive_envelope("text/plain",cue,{1,1,"native","agent","application/json",raw},7,1);
   CHECK(second.recalled.matches().size()==1 && second.recalled.matches()[0].original==saved);
   CHECK(host.input("text/plain",cue).matches().size()==2);
   host.end_session();CHECK(host.work(7,1)==1);
  }
  CHECK(memory.used()==0);
  {
   auto host=Runtime::open(path,cfg,memory);host.start_session(id(92),"reader");
   auto recall=host.input("text/plain",cue);
   CHECK(!recall.temporary() && recall.matches().size()==2);
   CHECK(recall.matches()[0].original==saved);
   auto replay=host.replay(recall,0);
   CHECK(std::ranges::equal(evidence_payload(replay.original()).content,raw));
  }
 }
 {
  const auto path=root/"delivery-stream";fs::create_directory(path);
  auto cfg=config;cfg.session_block_capacity=8<<20;cfg.read_limit=8<<20;
  MemoryBudget bounded(256<<10);
  const std::string payload(2<<20,'z');const auto raw=std::as_bytes(std::span(payload));
  const std::string prompt="small input";const auto cue=std::as_bytes(std::span(prompt));
  ExperienceLocation saved;
  {
   auto host=Runtime::create(path,cfg,bounded);host.start_session(id(93),"stream");
   saved=host.receive_envelope("text/plain",cue,{0,42,"stream","codex/hook","application/json",raw},7,0).recorded.original;
  }
  CHECK(bounded.used()==0);
  {
   auto host=Runtime::open(path,cfg,bounded);host.resume_session(id(93));
   struct Expected{ExperienceLocation original;DigestBytes fingerprint;unsigned visits=0;};
   Expected expected{saved,agent_delivery_identity(0,42,payload)};
   const auto used=bounded.used();const auto before_writes=writes;
   host.session().visit_deliveries("stream","codex/hook","application/json",&expected,
    [](void* context,const OriginalDelivery& delivery){
     auto& expected=*static_cast<Expected*>(context);++expected.visits;
     CHECK(delivery.original()==expected.original && delivery.fingerprint()==expected.fingerprint);
     CHECK(delivery.sequence()==0);
    });
   CHECK(expected.visits==1 && bounded.used()==used && writes==before_writes);
   throws<std::invalid_argument>([&]{host.session().visit_deliveries("wrong","codex/hook","application/json",&expected,
    [](void*,const OriginalDelivery&){CHECK(false);});});
   throws<std::invalid_argument>([&]{host.session().visit_deliveries("stream","wrong","application/json",&expected,
    [](void*,const OriginalDelivery&){CHECK(false);});});
   throws<std::invalid_argument>([&]{host.session().visit_deliveries("stream","codex/hook","text/plain",&expected,
    [](void*,const OriginalDelivery&){CHECK(false);});});
   throws<std::bad_alloc>([&]{(void)host.replay(host.input("text/plain",cue),0);});
  }
  CHECK(bounded.used()==0);
 }
 {
  const auto path=root/"ensure";fs::create_directory(path);
  {auto host=Runtime::ensure(path,config,memory);host.start_session(id(201),"retained");(void)host.retain({0,1,"retained","fixture","text/plain",content},7,0);}
  {auto host=Runtime::ensure(path,config,memory);host.resume_session(id(201));CHECK(host.input("text/plain",content).matches().size()==1);
   throws<std::system_error>([&]{(void)Runtime::ensure(path,config,memory);});}
  // An interrupted initialization is preserved and never recreated.
  const auto broken=root/"ensure-broken";fs::create_directories(broken/"graph");
  const auto before=writes;
  throws<std::system_error>([&]{(void)Runtime::ensure(broken,config,memory);});CHECK(writes==before);
  const auto foreign=root/"ensure-foreign";fs::create_directories(foreign/"sessions");
  throws<std::runtime_error>([&]{(void)Runtime::ensure(foreign,config,memory);});CHECK(!fs::exists(foreign/"graph"));
 }
 {
  const auto path=root/"cognition-conflict";fs::create_directory(path);
  auto host=Runtime::create(path,config,memory);host.start_session(id(211),"conflict");
  host.define_connection(id(212));
  const auto observe=[&](unsigned n,EvidenceOutcome outcome){
   EvidenceObservation value;value.hypothesis=id(212);value.source=id(n+20);
   value.context=id(n+70);value.producer=id(n+120);value.observed_at=n;value.outcome=outcome;
   return host.observe(id(212),{n,n,"conflict","experiment","text/plain",content},value,7,n).original;
  };
  ExperienceLocation remembered;
  for(unsigned n=1;n<=16;++n)remembered=observe(n,EvidenceOutcome::support);
  auto recalled=host.input("text/plain",content);
  for(unsigned n=17;n<=32;++n)(void)observe(n,EvidenceOutcome::refute);
  const auto w=writes;
  const auto strength=host.session().find(id(212))->state().strength();
  auto cognition=host.cognize(recalled,7,32);
  CHECK(cognition&&cognition->candidate==15&&cognition->replayed.location()==remembered);
  CHECK(cognition->comparison.agreement()==ReplayAgreement::contradicts);
  CHECK(cognition->reverified.has_value());
  CHECK(cognition->assessment().agreement()==ReplayAgreement::contradicts);
  CHECK(cognition->assessment().current_originals().size()==16);
  CHECK(writes==w&&host.session().find(id(212))->state().strength()==strength);
 }
 {
  const auto path=root/"natural-dialogue";fs::create_directory(path);
  ExperienceLocation previous;
  {
   auto host=Runtime::create(path,config,memory);host.start_session(id(221),"dialogue");
   const std::array<std::string_view,3> turns{"Keep the original goal", "Implement the next function", "Now review the change"};
   for(std::size_t n=0;n<turns.size();++n){
    const auto bytes=std::as_bytes(std::span(turns[n]));
    const auto r=reads,w=writes;auto probe=host.input("text/plain",bytes);
    CHECK(reads==r&&writes==w&&probe.matches().size()==n);
    if(n)CHECK(probe.key_kind()==FamiliarityKey::context);
    auto received=host.receive({n,n+1,"dialogue","user","text/plain",bytes},7,n+1);
    auto cognition=host.cognize(received.recalled,7,n+1);
    if(n){
     CHECK(cognition&&cognition->replayed.location()==previous);
     CHECK(cognition->assessment().agreement()==ReplayAgreement::insufficient&&!cognition->reverified);
    }else CHECK(!cognition);
    previous=received.recorded.original;
   }
  }
  {
   auto host=Runtime::open(path,config,memory);host.resume_session(id(221));
   const std::string_view fresh="Continue after restarting";
   const auto r=reads,w=writes;auto recalled=host.input("text/plain",std::as_bytes(std::span(fresh)));
   CHECK(reads==r&&writes==w);
   CHECK(recalled.key_kind()==FamiliarityKey::context&&recalled.matches().size()==3);
   auto cognition=host.cognize(recalled,7,4);CHECK(cognition&&cognition->replayed.location()==previous);
  }
 }
 {
  const auto path=root/"targeted-end";fs::create_directory(path);
  auto host=Runtime::create(path,config,memory);
  host.start_session(id(20),"selected");
  const auto kept=host.retain({0,0,"selected","user","text/plain",content},7,0).original;
  auto recall=host.input("text/plain",content);
  host.attach_session(id(21),"other");
  const auto before=writes;
  throws<std::invalid_argument>([&]{host.end_session(id(22));});
  CHECK(writes==before && host.has_session() && host.attached_sessions()==2);
  CHECK(host.replay(recall,0).location()==kept);
  host.end_session(id(21));
  CHECK(host.has_session() && host.attached_sessions()==1);
  CHECK(host.replay(recall,0).location()==kept);
  CHECK(host.work(7,0)==0); // only the empty ended source is eligible
  host.attach_session(id(22),"failed-end");
  fail_write=true;
  throws<std::system_error>([&]{host.end_session(id(22));});
  CHECK(host.has_session() && host.attached_sessions()==2);
  CHECK(host.replay(recall,0).location()==kept);
  CHECK(host.retain({1,1,"selected","user","text/plain",content},7,0).original!=kept);
  host.end_session(id(20));CHECK(!host.has_session());
  host.attach_session(id(23),"unselected");
  host.end_session(id(23));CHECK(!host.has_session());
 }
 {
  const auto path=root/"main-pages";fs::create_directory(path);
  const auto cue=input_cue("text/plain",content);
  ExperienceLocation selected;std::uint64_t stored=0;
  {
   auto host=Runtime::create(path,config,memory);host.start_session(id(231),"page-source");
   for(unsigned n=0;n<31;++n){
    const auto original=host.retain({n,n,"page-source","user","text/plain",content},7,n).original;
    if(n==15)selected=original;
   }
   host.end_session();CHECK(host.work(7,31)==1);host.start_session(id(232),"page-reader");
   throws<std::out_of_range>([&]{(void)host.page_out_main(id(233),0);});
   throws<std::out_of_range>([&]{(void)host.page_out_main(cue,31);});
   CHECK(!fs::exists(path/"metadata-pages"));
   const auto generation=host.main().graph().generation();
   const auto strength=host.main().graph().find(cue)->strength();
   {
    auto pinned=host.input("text/plain",content);CHECK(!pinned.temporary()&&pinned.matches().size()==31);
    const auto bytes=host.storage().used();CHECK(!host.page_out_main(cue,15));
    CHECK(host.storage().used()==bytes);
   }
   // A failed first write must leave the resident original available.
   fail_write=true;throws<std::system_error>([&]{(void)host.page_out_main(cue,15);});
   {auto recalled=host.input("text/plain",content);CHECK(host.replay(recalled,15).location()==selected);}
   const auto resident=memory.used(),bytes=host.storage().used();
   const auto io=host.storage().transfer().requested();
   CHECK(host.page_out_main(cue,15));CHECK(memory.used()<resident);
   CHECK(host.storage().used()>bytes&&host.storage().transfer().requested()>io);
   CHECK(host.main().graph().generation()==generation&&host.main().graph().find(cue)->strength()==strength);
   {
    const auto r=reads,w=writes;const auto transfer=host.storage().transfer().requested();
    auto recalled=host.input("text/plain",content);
    CHECK(reads==r&&writes==w&&host.storage().transfer().requested()==transfer);
    CHECK(host.replay(recalled,15).location()==selected);CHECK(reads>r);
   }
   stored=host.storage().used();const auto w=writes;
   CHECK(host.page_out_main(cue,15));CHECK(host.storage().used()==stored&&writes==w);
  }
  {
   // Failed writes retain their attempted extent until cold inventory reconciles it.
   const auto actual=stored_bytes(path,memory);CHECK(actual<=stored);
   auto host=Runtime::open(path,config,memory);CHECK(host.storage().used()==actual);
   host.resume_session(id(232));auto recalled=host.input("text/plain",content);
   CHECK(!recalled.temporary()&&host.replay(recalled,15).location()==selected);
  }
 }
 {
  const auto path=root/"pressure";fs::create_directory(path);
  auto cfg=config;cfg.memory_target_bytes=1;
  auto host=Runtime::create(path,cfg,memory);host.start_session(id(240),"pressure-source");
  ExperienceLocation expected;
  for(unsigned n=0;n<31;++n){
   const auto value=host.retain({n,n,"pressure-source","user","text/plain",content},7,n).original;
   if(n==15)expected=value;
  }
  host.end_session();CHECK(host.work(7,31)==1);host.start_session(id(241),"pressure-reader");
  const auto drain=[&]{unsigned steps=0;while(host.maintain_memory()){CHECK(++steps<40);}return steps;};
  {
   auto pinned=host.input("text/plain",content);const auto bytes=host.storage().used();
   CHECK(drain()>0);CHECK(host.storage().used()==bytes);
   CHECK(!fs::exists(path/"metadata-pages"));
  }
  const auto before=memory.used();CHECK(drain()>0);CHECK(memory.used()<before);
  const auto bytes=host.storage().used();CHECK(drain()>0);CHECK(host.storage().used()==bytes);
  const auto r=reads;auto recalled=host.input("text/plain",content);CHECK(reads==r);
  CHECK(host.replay(recalled,15).location()==expected);
 }
 CHECK(memory.used()==0);fs::remove_all(root);std::printf("runtime lifecycle tests: %u checks passed\n",checks);
}
