#include "vrs/portal_page.hpp"
#include "vrs/runtime.hpp"
#include "vrs/storage_inventory.hpp"
#include "swegca_architecture/input_cue.hpp"
#include "swegca_architecture/agent_delivery_identity.hpp"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <sys/wait.h>
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
  // Live observations reach the same core refinement before session end or any
  // Main merge. No prior processed experience, observe call or DLM is required.
  const auto path=root/"live-evidence";fs::create_directory(path);
  auto live_config=config;live_config.policy=EvidencePolicy{};
  auto host=Runtime::create(path,live_config,memory);host.start_session(id(12),"live");
  for(const auto outcome:{EvidenceOutcome::support,EvidenceOutcome::refute,EvidenceOutcome::insufficient}){
   const std::string cue="live predicate "+std::to_string(static_cast<unsigned>(outcome));
   const auto bytes=std::as_bytes(std::span(cue));
   EvidenceStatus last=EvidenceStatus::abstain;
   for(unsigned n=0;n<48;++n){
    EvidenceObservation value;value.axis=n%4;value.source=id(100+n);value.producer=id(150+n);
    value.context=id(200+n);value.outcome=outcome;value.producer_confidence=1;
    auto received=host.receive({n,0,"live","sensor","text/plain",bytes},7,0,&value);
    CHECK(received.recalled.matches().size()==n);
    const auto stored=host.session().read_original(received.recorded.original);
    const auto decoded=decode_evidence(make_evidence_rules(live_config.policy),stored);
    CHECK(decoded.value().outcome==outcome&&decoded.value().source==value.source);
    CHECK(decoded.value().context==value.context&&decoded.value().producer==value.producer);
    CHECK(std::ranges::equal(evidence_payload(stored).content,bytes));
    CHECK(host.main().graph().generation()==0&&host.session().phase()==SessionPhase::active);
    last=received.recorded.refinement.result().verification().judgment().status();
   }
   CHECK(last==(outcome==EvidenceOutcome::support?EvidenceStatus::accept:
       outcome==EvidenceOutcome::refute?EvidenceStatus::reject:EvidenceStatus::abstain));
  }
  // Repeated input from the same observational group is not new independence.
  EvidenceObservation repeated;repeated.source=id(60);repeated.producer=id(61);
  repeated.context=id(62);repeated.outcome=EvidenceOutcome::support;repeated.producer_confidence=1;
  for(unsigned n=0;n<12;++n){
   auto event=host.receive({n,0,"live","sensor","text/plain",content},7,0,&repeated);
   CHECK(event.recorded.refinement.result().verification().judgment().status()==EvidenceStatus::abstain);
  }
  // Expired current-input observations are preserved but cannot contribute.
  const std::string expired_text="expired live predicate";
  const auto expired_bytes=std::as_bytes(std::span(expired_text));
  for(unsigned n=0;n<48;++n){
   EvidenceObservation value;value.axis=n%4;value.source=id(100+n);value.producer=id(150+n);
   value.context=id(200+n);value.outcome=EvidenceOutcome::support;value.producer_confidence=1;
   value.has_expiry=true;value.expires_at=0;
   auto event=host.receive({n,0,"live","sensor","text/plain",expired_bytes},7,1,&value);
   CHECK(event.recorded.refinement.result().verification().judgment().status()==EvidenceStatus::abstain);
  }
  // Bad observation metadata must not poison the owner or write any prefix.
  for(unsigned kind=0;kind<4;++kind){
   auto bad=repeated;
   if(kind==0)bad.source={};
   if(kind==1)bad.axis=4;
   if(kind==2)bad.observed_at=1;
   if(kind==3)bad.producer_confidence=2;
   const auto before_bad=writes;
   throws<std::invalid_argument>([&]{(void)host.receive({0,0,"live","sensor","text/plain",content},7,0,&bad);});
   CHECK(writes==before_bad&&host.session().usable());
  }
  EvidenceObservation invalid;invalid.hypothesis=id(9);
  const auto before=writes;
  throws<std::invalid_argument>([&]{(void)host.receive({0,0,"live","sensor","text/plain",content},7,0,&invalid);});
  CHECK(writes==before&&host.session().usable());
 }
 {
  // Reopen an active session: evidence does not require end/merge to survive.
  auto live_config=config;live_config.policy=EvidencePolicy{};
  auto host=Runtime::open(root/"live-evidence",live_config,memory);host.resume_session(id(12));
  CHECK(host.session().phase()==SessionPhase::active&&host.main().graph().generation()==0);
  for(const auto outcome:{EvidenceOutcome::support,EvidenceOutcome::refute,EvidenceOutcome::insufficient}){
   const std::string cue="live predicate "+std::to_string(static_cast<unsigned>(outcome));
   const auto bytes=std::as_bytes(std::span(cue));
   auto recalled=host.input("text/plain",bytes);CHECK(recalled.temporary()&&recalled.matches().size()==48);
   auto replayed=host.replay(recalled,0);
   const auto value=decode_evidence(make_evidence_rules(live_config.policy),replayed.original()).value();
   CHECK(value.outcome==outcome&&named_digest(value.source)&&named_digest(value.context));
   const auto* connection=host.session().find(value.hypothesis);CHECK(connection);
   const double strength=connection->snapshot().strength;
   CHECK(outcome==EvidenceOutcome::support?strength>1:outcome==EvidenceOutcome::refute?strength<1:strength==1);
   std::printf("live input restart: outcome=%u originals=48 strength=%.9f main_generation=0\n",
       static_cast<unsigned>(outcome),strength);
  }
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
  const auto head=host.session().find(id(212))->head();
  const auto strength=host.session().find(id(212))->state().strength();
  auto cognition=host.cognize(recalled,7,32);
  CHECK(cognition&&cognition->candidate==15&&cognition->replayed.location()==remembered);
  CHECK(cognition->comparison.agreement()==ReplayAgreement::contradicts);
  CHECK(cognition->reverified.has_value());
  CHECK(cognition->assessment().agreement()==ReplayAgreement::contradicts);
  CHECK(cognition->assessment().current_originals().size()==16);
  CHECK(host.session().find(id(212))->head()==head&&host.session().find(id(212))->state().strength()==strength);
  CHECK(host.session().read_replay_position()->original==remembered);
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
  const auto path=root/"same-name-contexts";fs::create_directory(path);
  const std::string a="owner A first",a2="owner A second",b="owner B first",fresh="continue this dialogue";
  ExperienceLocation address_a,address_a2,address_b;
  {
   auto host=Runtime::create(path,config,memory);host.start_session(id(230),"same-name");
   address_a=host.retain({0,0,"same-name","user","text/plain",std::as_bytes(std::span(a))},7,0).original;
   address_a2=host.retain({1,1,"same-name","user","text/plain",std::as_bytes(std::span(a2))},7,1).original;
   host.end_session();CHECK(host.work(7,1)==1);
   host.start_session(id(231),"same-name");
   address_b=host.retain({0,0,"same-name","user","text/plain",std::as_bytes(std::span(b))},7,0).original;
   host.end_session();CHECK(host.work(7,1)==1);
   host.start_session(id(232),"reader");
  }
  {
   auto host=Runtime::open(path,config,memory);host.resume_session(id(232));
   CHECK(host.replay(host.input("text/plain",std::as_bytes(std::span(a))),0).location()==address_a);
   auto related=host.input("text/plain",std::as_bytes(std::span(fresh)));
   CHECK(!related.temporary()&&related.matches().size()==2);
   for(const auto& match:related.matches())CHECK(match.original==address_a||match.original==address_a2);
   CHECK(host.replay(host.input("text/plain",std::as_bytes(std::span(b))),0).location()==address_b);
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
  const auto path=root/"sparse-main-page";fs::create_directory(path);
  auto host=Runtime::create(path,config,memory);host.start_session(id(234),"sparse-source");
  host.define_connection(id(236));ExperienceLocation selected;
  for(unsigned n=0;n<31;++n){
   const auto other="other-sparse-cue-"+std::to_string(n);
   const auto payload=n==15?content:std::as_bytes(std::span(other));
   EvidenceObservation value;value.hypothesis=id(236);value.source=id(n+20);
   value.context=id(n+70);value.producer=id(n+120);value.observed_at=n;value.outcome=EvidenceOutcome::support;
   const auto original=host.observe(id(236),{n,n,"sparse-source","experiment","text/plain",payload},value,7,n).original;
   if(n==15)selected=original;
  }
  host.end_session();CHECK(host.work(7,31)==1);host.start_session(id(235),"sparse-reader");
  CHECK(host.page_out_main(id(236),15));const auto cold=memory.used();
  {
   auto recalled=host.input("text/plain",content);
   CHECK(!recalled.temporary()&&recalled.matches().size()==1);
   CHECK(recalled.matches()[0].original==selected);
   CHECK(memory.used()-cold<16*sizeof(ExperienceEvidence));
   const auto receipt_bytes=memory.used();
   CHECK(host.select_replay(recalled)==0);
   CHECK(host.replay(recalled,0).location()==selected);
   CHECK(memory.used()==receipt_bytes);
  }
  CHECK(memory.used()==cold);
  CHECK(!host.page_out_main(id(236),15));
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
    const auto cold_bytes=memory.used();
    const auto selection_reads=reads;
    fail_read=true;CHECK(host.select_replay(recalled)==30);
    {
     const auto connections=host.select_replay_connections(recalled,2);
     CHECK(connections.entries.size()==1&&!connections.next);
     CHECK(connections.entries.front().candidate==30);
     CHECK(reads==selection_reads&&fail_read);
    }

    CHECK(reads==selection_reads&&fail_read); // Complete page selection needs no I/O.
    throws<std::system_error>([&]{(void)host.replay(recalled,15);});
    CHECK(memory.used()==cold_bytes);
    CHECK(host.select_replay(recalled)==30);
    CHECK(memory.used()==cold_bytes&&writes==w);
    CHECK(host.select_replay(recalled)==30&&memory.used()==cold_bytes);
    CHECK(host.replay(recalled,15).location()==selected);CHECK(reads>r);
    CHECK(memory.used()==cold_bytes);
    CHECK(recalled.matches()[15].original==selected&&memory.used()==cold_bytes);
    {auto partial=host.read_payload_slice(recalled,15,0,1);CHECK(partial.evidence().original()==selected);}
    CHECK(memory.used()==cold_bytes);
   }
   stored=host.storage().used();const auto w=writes;
   CHECK(!host.page_out_main(cue,15));CHECK(host.storage().used()==stored&&writes==w);
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
  // Exhausting an empty Main must not hide a merge arriving before the final
  // idle callback: that callback otherwise returns false and blocks forever.
  const auto path=root/"pressure-generation";fs::create_directory(path);
  auto cfg=config;cfg.memory_target_bytes=1;
  auto host=Runtime::create(path,cfg,memory);
  host.start_session(id(230),"generation-source"); // live session supplies RAM pressure
  CHECK(host.maintain_memory()); // empty large-segment pass
  CHECK(host.maintain_memory()); // empty shared-page pass; completion pending
  for(unsigned n=0;n<31;++n)(void)host.retain({n,n,"generation-source","user","text/plain",content},7,n);
  host.end_session();CHECK(host.work(7,31)==1);
  CHECK(host.maintain_memory()); // new generation must restart, not report done
  unsigned steps=0;
  while(host.maintain_memory()){CHECK(++steps<5000);std::this_thread::sleep_for(std::chrono::milliseconds(1));}
  CHECK(fs::exists(path/"metadata-pages")&&!fs::is_empty(path/"metadata-pages"));
 }
 {
  const auto path=root/"singleton-pressure";fs::create_directory(path);
  auto cfg=config;cfg.memory_target_bytes=1;
  std::array<ExperienceLocation,64> originals;
  const auto input_text=[](unsigned n){return "singleton pressure input "+std::to_string(n);};
  {
   auto host=Runtime::create(path,cfg,memory);host.start_session(id(238),"singleton-source");
   for(unsigned n=0;n<originals.size();++n){
    const auto value=input_text(n);
    originals[n]=host.receive({n,n,"singleton-source","user","text/plain",std::as_bytes(std::span(value))},7,n).recorded.original;
   }
   host.end_session();CHECK(host.work(7,64)==1);host.start_session(id(239),"singleton-reader");
   const auto generation=host.main().graph().generation();const auto resident=memory.used();
   unsigned steps=0;
   while(host.maintain_memory()){CHECK(++steps<5000);std::this_thread::sleep_for(std::chrono::milliseconds(1));}
   CHECK(memory.used()<resident&&host.main().graph().generation()==generation);
   CHECK(std::distance(fs::directory_iterator(path/"metadata-pages"),fs::directory_iterator{})==1);
   const auto cold=memory.used();
   for(unsigned n=0;n<originals.size();++n){
    const auto value=input_text(n);const auto r=reads,w=writes;
    auto recalled=host.input("text/plain",std::as_bytes(std::span(value)));
    CHECK(reads==r&&writes==w&&!recalled.temporary()&&recalled.matches().size()==1);
    CHECK(host.select_replay(recalled)==0&&reads==r);
    CHECK(host.replay(recalled,0).location()==originals[n]);
   }
   CHECK(memory.used()==cold);
   {
    const auto r=reads,w=writes;
    auto contextual=host.input("text/followup",content);
    CHECK(contextual.key_kind()==FamiliarityKey::context&&contextual.matches().size()==64);
    CHECK(reads==r&&writes==w);
    const auto chosen=host.select_replay(contextual);CHECK(chosen.has_value()&&reads==r);
    CHECK(host.replay(contextual,*chosen).location()==originals.back());
   }
   CHECK(memory.used()==cold);
   // A subsequent real merge reads sealed slices and retains all old originals.
   const auto value=input_text(17);
   (void)host.receive({0,100,"singleton-reader","user","text/plain",std::as_bytes(std::span(value))},7,100);
   host.end_session();CHECK(host.work(7,100)==1);
   host.start_session(id(237),"singleton-after-merge");
   auto recalled=host.input("text/plain",std::as_bytes(std::span(value)));
   CHECK(recalled.matches().size()==2&&host.replay(recalled,0).location()==originals[17]);
  }
  {
   auto host=Runtime::open(path,cfg,memory);host.resume_session(id(237));
   const auto value=input_text(17);auto recalled=host.input("text/plain",std::as_bytes(std::span(value)));
   CHECK(recalled.matches().size()==2&&host.replay(recalled,0).location()==originals[17]);
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
  const auto drain=[&]{unsigned steps=0;while(host.maintain_memory()){CHECK(++steps<5000);std::this_thread::sleep_for(std::chrono::milliseconds(1));}return steps;};
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
 {
  const auto path=root/"pressure";
  auto cfg=config;cfg.memory_target_bytes=1;cfg.storage_bytes=stored_bytes(path,memory);
  auto host=Runtime::open(path,cfg,memory);host.resume_session(id(241));
  const auto cue=input_cue("text/plain",content);
  const auto expected=host.main().graph().find(cue)->experiences()[15].original();
  const auto bytes=host.storage().used();
  unsigned attempts=0;
  while(host.maintain_memory()){CHECK(++attempts<5000);std::this_thread::sleep_for(std::chrono::milliseconds(1));}
  CHECK(attempts>0&&host.storage().used()==bytes);
  CHECK(host.replay(host.input("text/plain",content),15).location()==expected);
  CHECK(fs::is_empty(path/"metadata-pages"));
 }
 {
  const auto path=root/"pressure";std::optional<InputRecall> retained;
  {
   auto host=Runtime::open(path,config,memory);host.resume_session(id(241));
   CHECK(host.page_out_main(input_cue("text/plain",content),15));
   retained.emplace(host.input("text/plain",content));
  }
  CHECK(!fs::is_empty(path/"metadata-pages"));
  {
   auto reopened=Runtime::open(path,config,memory);
   CHECK(!fs::is_empty(path/"metadata-pages")); // The old page handle is still locked.
  }
  // Expired receipts are never used for a read, but their destruction is safe.
  retained.reset();CHECK(fs::is_empty(path/"metadata-pages"));
 }
 {
  const auto path=root/"crash-pages";fs::create_directory(path);
  const auto child=::fork();CHECK(child>=0);
  if(child==0){
   try {
    auto host=Runtime::create(path,config,memory);host.start_session(id(234),"crash-source");
    for(unsigned n=0;n<31;++n)(void)host.retain({n,n,"crash-source","user","text/plain",content},7,n);
    host.end_session();(void)host.work(7,31);host.start_session(id(235),"crash-reader");
    if(!host.page_out_main(input_cue("text/plain",content),15))::_exit(2);
    ::_exit(0); // No destructors: leave the completed derived page behind.
   }catch(...){::_exit(3);}
  }
  int status=0;CHECK(::waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0);
  const auto directory=path/"metadata-pages";CHECK(!fs::is_empty(directory));
  const auto orphan=fs::directory_iterator(directory)->path();
  const auto orphan_bytes=fs::file_size(orphan);
  const auto partial=directory/(std::string(64,'f')+".block");
  {const auto fd=::open(partial.c_str(),O_CREAT|O_EXCL|O_WRONLY,0600);CHECK(fd>=0);CHECK(::write(fd,"x",1)==1);CHECK(::close(fd)==0);}
  const auto original_path=directory/(std::string(64,'e')+".block");
  {
   DigestBytes identity;identity.fill(std::byte{0xee});
   auto original=ExperienceBlock::create(original_path,identity,
       ExperienceBlock::header_bytes+ExperienceBlock::record_overhead+1+1+10+content.size());
   (void)original.append({0,0,"s","x","text/plain",content});
  }
  const auto before=stored_bytes(path,memory);
  auto recovered_config=config;recovered_config.storage_bytes=before-orphan_bytes;
  {
   const auto prior_writes=writes;
   auto host=Runtime::open(path,recovered_config,memory);
   CHECK(writes==prior_writes);
   CHECK(!fs::exists(orphan)&&fs::exists(partial)&&fs::exists(original_path));
   CHECK(host.storage().used()==before-orphan_bytes);
   CHECK(host.storage().used()==host.storage().limit());
   throws<StorageLimit>([&]{host.start_session(id(236),"over-quota-denied");});
   host.resume_session(id(235));auto recalled=host.input("text/plain",content);
   CHECK(recalled.matches().size()==31&&host.select_replay(recalled)==30);
   const auto selected_page=host.select_replay_connections(recalled,8);
   CHECK(selected_page.entries.size()==1&&selected_page.entries[0].candidate==30);
   // An advancing Replay now commits its continuation position. At the hard
   // storage limit it must report that failure, while archive reads remain
   // available and must not relax the quota or pretend persistence succeeded.
   const auto original=recalled.matches()[15].original;
   throws<StorageLimit>([&]{(void)host.replay(recalled,15);});
   CHECK(host.storage().used()==host.storage().limit());
   auto archived=host.read_cognition_original(id(234),original);
   const auto restored=evidence_payload(archived).content;
   CHECK(restored.size()==content.size()&&std::equal(restored.begin(),restored.end(),content.begin()));
  }
  --recovered_config.storage_bytes;
  const auto used=stored_bytes(path,memory);const auto prior_writes=writes;
  throws<StorageLimit>([&]{(void)Runtime::open(path,recovered_config,memory);});
  CHECK(writes==prior_writes&&stored_bytes(path,memory)==used);
  CHECK(fs::exists(partial)&&fs::exists(original_path));
 }
 {
  // Recovery admission must not turn unsigned remaining-space subtraction
  // into permission to write when existing bytes already exceed the limit.
  throws<StorageLimit>([]{StorageBudget ordinary(1,2);});
  const auto path=root/"over-quota-create";fs::create_directory(path);
  const auto marker=path/"retained-original";
  {const auto fd=::open(marker.c_str(),O_CREAT|O_EXCL|O_WRONLY,0600);CHECK(fd>=0);CHECK(::ftruncate(fd,64)==0);CHECK(::close(fd)==0);}
  auto tiny=config;tiny.storage_bytes=1;
  const auto prior_writes=writes;
  throws<StorageLimit>([&]{(void)Runtime::create(path,tiny,memory);});
  CHECK(writes==prior_writes&&stored_bytes(path,memory)==64&&fs::file_size(marker)==64);
 }
 {
  const auto path=root/"inventory-many";fs::create_directory(path);
  std::uint64_t expected=0;
  for(unsigned n=0;n<512;++n){
   const auto file=path/std::to_string(n);
   const auto fd=::open(file.c_str(),O_CREAT|O_EXCL|O_RDWR|O_CLOEXEC,0600);CHECK(fd>=0);
   CHECK(::ftruncate(fd,n%17)==0);CHECK(::close(fd)==0);expected+=n%17;
  }
  StorageRoot owner(path);MemoryBudget inventory_budget(20000);
  CHECK(stored_bytes(path,inventory_budget)==expected&&inventory_budget.used()==0);
  const auto aliases=path/"aliases";fs::create_directory(aliases);
  for(unsigned n=0;n<64;++n)fs::create_hard_link(path/"1",aliases/std::to_string(n));
  // A link outside this inventory does not make its one inside name free.
  fs::create_hard_link(path/"2",root/"outside-inventory-link");
  MemoryBudget shared_budget(20000);
  CHECK(stored_bytes(path,shared_budget)==expected&&shared_budget.used()==0);
  CHECK(shared_budget.peak_reserved()<=20000&&shared_budget.peak_reserved()>0);
  MemoryBudget singleton_budget(1);
  throws<std::bad_alloc>([&]{(void)stored_bytes(path,singleton_budget);});
  CHECK(singleton_budget.used()==0);
  std::printf("inventory 512 inode peak: %zu bytes\n",shared_budget.peak_reserved());
 }
 {
  const auto path=root/"scope-original-binding";fs::create_directory(path);
  ExperienceLocation input,selected;DigestBytes connection;
  {
   auto host=Runtime::create(path,config,memory);host.start_session(id(240),"scope");
   input=host.retain({0,0,"scope","user","text/plain",content},7,0).original;
   EvidenceObservation value;value.source=id(241);value.producer=id(242);
   value.observed_at=1;value.outcome=EvidenceOutcome::support;
   auto result=host.observe_input_scope(input,"content",{1,1,"scope","tool","text/plain",content},value,7,1);
   selected=result.original;connection=result.refinement.connection();
  }
  auto host=Runtime::open(path,config,memory);host.resume_session(id(240));
  CHECK(host.read_scoped_cognition_original(input,"content",connection,id(240),selected).location()==selected);
  throws<std::invalid_argument>([&]{(void)host.read_scoped_cognition_original(input,"permissions",connection,id(240),selected);});
  throws<std::invalid_argument>([&]{(void)host.read_scoped_cognition_original(input,"content",id(243),id(240),selected);});
  throws<std::invalid_argument>([&]{(void)host.read_scoped_cognition_original(input,"content",connection,id(240),input);});
  throws<std::invalid_argument>([&]{(void)host.read_scoped_cognition_original(input,"content",connection,id(244),selected);});
  throws<std::invalid_argument>([&]{(void)host.read_scoped_cognition_original(input,"",connection,id(240),selected);});
 }
 {
  const auto path=root/"related-observations";fs::create_directory(path);
  ExperienceLocation input,positive,negative,uncertain,general;
  {
   auto host=Runtime::create(path,config,memory);host.start_session(id(241),"related");
   input=host.receive({0,0,"related","user","text/plain",content},7,0).recorded.original;
   auto parent_receipt=host.input("text/plain",content);
   auto parent=host.replay(parent_receipt,0);CHECK(parent.location()==input);
   auto absent=host.related(parent);CHECK(!absent.familiar()&&absent.matches().empty());
   EvidenceObservation value;value.source=id(242);value.producer=id(243);
   value.observed_at=1;value.outcome=EvidenceOutcome::support;
   auto first=host.observe_input_scope(input,"contents",{1,1,"related","tool","text/plain",content},value,7,1);
   positive=first.original;
   value.observed_at=2;value.outcome=EvidenceOutcome::refute;
   auto second=host.observe_input_scope(input,"permissions",{2,2,"related","tool","text/plain",content},value,7,2);
   negative=second.original;
   value.observed_at=1;value.outcome=EvidenceOutcome::insufficient;
   uncertain=host.observe_input_scope(input,"unmeasured",{3,1,"related","tool","text/plain",content},value,7,2).original;
   host.define_connection(id(250));value.hypothesis=id(250);value.context=input.digest;value.observed_at=5;
   const std::string response="general turn response";
   general=host.observe(id(250),{4,5,"related","server","text/plain",std::as_bytes(std::span(response))},value,7,5).original;
   CHECK(!decode_evidence(host.session().find(id(250))->rules(),host.session().read_original(general)).has_input_key());
   value.hypothesis={};value.context={};value.outcome=EvidenceOutcome::refute;
   const std::string other_text="a different purpose";const auto other_bytes=std::as_bytes(std::span(other_text));
   const auto other=host.receive({3,3,"related","user","text/plain",other_bytes},7,3).recorded.original;
   value.observed_at=4;
   const auto unrelated=host.observe_input_scope(other,"contents",{4,4,"related","tool","text/plain",content},value,7,4).original;
   const auto before_reads=reads,before_writes=writes;
   auto linked=host.related(parent);
   CHECK(linked.temporary()&&linked.key_kind()==FamiliarityKey::context&&linked.lookup_key()==input.digest);
   CHECK(linked.seed_only()&&linked.matches().size()==3&&reads==before_reads&&writes==before_writes);
   bool saw_positive=false,saw_negative=false,saw_uncertain=false;
   for(const auto match:linked.matches()){
    saw_positive|=match.original==positive;saw_negative|=match.original==negative;
    saw_uncertain|=match.original==uncertain;
    CHECK(match.original!=unrelated&&match.original!=input&&match.original!=general);
   }
   CHECK(saw_positive&&saw_negative&&saw_uncertain);
   const auto reads_before_page=reads,writes_before_page=writes;
   const auto full=host.select_replay_connections(linked,8);
   CHECK(full.entries.size()==3&&!full.next);
   std::optional<DigestBytes> cursor;std::size_t index=0;
   do{
    const auto page=host.select_replay_connections(linked,1,cursor?&*cursor:nullptr);
    CHECK(page.entries.size()==1);
    CHECK(page.entries[0].connection==full.entries[index].connection);
    CHECK(page.entries[0].candidate==full.entries[index].candidate);
    CHECK(page.entries[0].candidate==*host.select_replay(linked,&page.entries[0].connection));
    CHECK(linked.matches()[page.entries[0].candidate].recalled.recalled_head.identity==page.entries[0].connection);
    ++index;cursor=page.next;
   }while(cursor);
   CHECK(index==3&&reads==reads_before_page&&writes==writes_before_page);
   const auto end=host.select_replay_connections(linked,1,&full.entries.back().connection);
   CHECK(end.entries.empty()&&!end.next);
   throws<std::invalid_argument>([&]{(void)host.select_replay_connections(linked,0);});

   const auto observed_head=host.session().find(second.refinement.connection())->head();
   auto cognition=host.cognize(linked,7,4);
   CHECK(cognition&&cognition->replayed.location()==negative);
   CHECK(evidence_payload(cognition->replayed.original()).content.size()==content.size());
   CHECK(host.session().find(second.refinement.connection())->head()==observed_head);
   CHECK(host.session().read_replay_position()->original==negative);
   const auto saved_writes=writes;(void)host.cognize(linked,7,4);CHECK(writes==saved_writes);
   host.attach_session(id(244),"other-route");host.select_session(id(244));
   throws<std::invalid_argument>([&]{(void)host.related(parent);});
   throws<std::invalid_argument>([&]{(void)host.select_replay_connections(linked,1);});
   host.select_session(id(241));host.end_session();CHECK(host.work(7,5)==1);
  }
  {
   auto host=Runtime::open(path,config,memory);host.start_session(id(245),"related-main");
   auto recalled=host.input("text/plain",content);CHECK(!recalled.temporary());
   // Scoped results have distinct cues, so the parent input remains the exact match.
   CHECK(recalled.matches().size()==1);
   auto parent=host.replay(recalled,0);CHECK(parent.location()==input);
   const auto before_reads=reads,before_writes=writes;
   auto linked=host.related(parent);CHECK(!linked.temporary()&&linked.seed_only()&&linked.matches().size()==3);
   const auto main_page=host.select_replay_connections(linked,2);
   CHECK(main_page.entries.size()==2&&main_page.next);
   const auto main_tail=host.select_replay_connections(linked,2,&*main_page.next);
   CHECK(main_tail.entries.size()==1&&!main_tail.next);
   CHECK(main_page.entries.back().connection<main_tail.entries.front().connection);

   CHECK(linked.lookup_key()==input.digest&&reads==before_reads&&writes==before_writes);
   const auto main_head=host.main().head();
   auto selected=host.cognize(linked,7,5);CHECK(selected&&selected->replayed.location()==negative);
   CHECK(host.main().head()==main_head&&host.session().read_replay_position()->original==negative);
   // Local general dialogue cannot hide eligible observations in Main.
   host.define_connection(id(251));EvidenceObservation value;
   value.hypothesis=id(251);value.context=input.digest;value.source=id(21);value.producer=id(22);value.observed_at=6;
   (void)host.observe(id(251),{0,6,"related-main","server","text/plain",content},value,7,6);
   auto fallback=host.related(parent);CHECK(!fallback.temporary()&&fallback.matches().size()==3);
   CHECK(host.cognize(fallback,7,6)->replayed.location()==negative);
   DigestBytes contents_connection{},permissions_connection{};
   for(const auto match:linked.matches()){
    if(match.original==positive)contents_connection=match.recalled.recalled_head.identity;
    if(match.original==negative)permissions_connection=match.recalled.recalled_head.identity;
   }
   const auto local_path=path/"connection-fallback";fs::create_directory(local_path);
   auto local_store=SessionStore::create(local_path,id(253),"related-main",65536,memory);
   SessionRuntime local(local_store,memory,8192);
   ExperienceRouter route(local,memory);route.mount_main(host.main());
   auto parent_candidates=route.input("text/plain",content);
   auto routed_parent=route.replay(parent_candidates,0);CHECK(routed_parent.location()==input);
   local.define_connection(contents_connection,1.0,policy);
   value.hypothesis=contents_connection;value.context=input.digest;value.observed_at=7;value.outcome=EvidenceOutcome::insufficient;
   const auto local_observation=local.observe(contents_connection,
       {1,7,"related-main","tool","text/plain",content},value,7,7,contents_connection).original;
   const auto read_boundary=reads,write_boundary=writes;
   auto local_only=route.related(routed_parent,&contents_connection);
   CHECK(local_only.temporary()&&local_only.matches().size()==1);
   CHECK(local_only.matches()[0].original==local_observation);
   auto main_only=route.related(routed_parent,&permissions_connection);
   CHECK(!main_only.temporary()&&main_only.matches().size()==1);
   CHECK(main_only.matches()[0].original==negative);
   const auto unknown_connection=id(252);
   CHECK(route.related(routed_parent,&unknown_connection).matches().empty());
   CHECK(reads==read_boundary&&writes==write_boundary);
   CHECK(route.replay(main_only,0).location()==negative);
   CHECK(route.replay(local_only,0).location()==local_observation);
   const auto listing_reads=reads,listing_writes=writes;
   const auto combined=route.related_connections(routed_parent,8);
   CHECK(combined.entries.size()==3&&!combined.next&&combined.mixed_tiers&&!combined.temporary);
   bool saw_local=false,saw_main=false;
   for(const auto& entry:combined.entries){
    if(entry.connection==contents_connection){CHECK(entry.temporary&&entry.original==local_observation);saw_local=true;}
    if(entry.connection==permissions_connection){CHECK(!entry.temporary&&entry.original==negative);saw_main=true;}
   }
   CHECK(saw_local&&saw_main);
   std::optional<DigestBytes> after;std::size_t page_index=0;
   do{
    const auto page=route.related_connections(routed_parent,1,after?&*after:nullptr);
    CHECK(page.snapshot==combined.snapshot&&page.entries.size()==1&&!page.mixed_tiers);
    CHECK(page.entries[0].connection==combined.entries[page_index].connection);
    CHECK(page.entries[0].original==combined.entries[page_index].original);
    CHECK(page.entries[0].temporary==combined.entries[page_index].temporary);
    ++page_index;after=page.next;
   }while(after);
   CHECK(page_index==3&&reads==listing_reads&&writes==listing_writes);
   CHECK(route.related_connections(routed_parent,1,&combined.entries.back().connection).entries.empty());
   throws<std::invalid_argument>([&]{(void)route.related_connections(routed_parent,0);});
   value.observed_at=8;
   (void)local.observe(contents_connection,{2,8,"related-main","tool","text/plain",content},value,7,8,contents_connection);
   CHECK(route.related_connections(routed_parent,8).snapshot!=combined.snapshot);


  }
 }
 {
  const auto path=root/"scope-live-restore";fs::create_directory(path);
  ExperienceLocation input,selected,head;DigestBytes connection;
  const auto observe=[&](Runtime& host,unsigned n,EvidenceOutcome outcome){
   const auto parent=host.retain({n,n,"restore","user","text/plain",content},7,n).original;
   EvidenceObservation value;value.source=id(n+20);value.producer=id(n+60);
   value.observed_at=n;value.outcome=outcome;
   auto recorded=host.observe_input_scope(parent,"contents",{n,n,"restore","tool","text/plain",content},value,7,n);
   input=parent;connection=recorded.refinement.connection();return recorded.original;
  };
  {
   auto host=Runtime::create(path,config,memory);host.start_session(id(245),"restore");
   for(unsigned n=1;n<=8;++n)selected=observe(host,n,EvidenceOutcome::support);
   head=host.session().find(connection)->head();
  }
  const auto remembered_input=input;
  auto host=Runtime::open(path,config,memory);host.resume_session(id(245));
  auto initially=host.restore_temporary_cognition(remembered_input,"contents",connection,head,7,selected,7,8);
  CHECK(initially.replayed.location()==selected&&initially.comparison.observation_boundary()==8);
  CHECK(initially.assessment().current_originals().empty()&&!initially.reverified);
  for(unsigned n=9;n<=16;++n)(void)observe(host,n,EvidenceOutcome::refute);
  const auto before=host.session().find(connection)->snapshot();
  auto restored=host.restore_temporary_cognition(remembered_input,"contents",connection,head,7,selected,7,16);
  CHECK(restored.replayed.location()==selected&&restored.reverified.has_value());
  CHECK(restored.assessment().current_originals().size()==8&&restored.comparison.observation_boundary()==8);
  CHECK(restored.assessment().verification().result().verification().judgment().status()==EvidenceStatus::reject);
  CHECK(host.session().find(connection)->snapshot().record==before.record);
  throws<std::invalid_argument>([&]{(void)host.restore_temporary_cognition(remembered_input,"other",connection,head,7,selected,7,16);});
  throws<std::invalid_argument>([&]{(void)host.restore_temporary_cognition(remembered_input,"contents",connection,head,8,selected,7,16);});
  throws<std::invalid_argument>([&]{(void)host.restore_temporary_cognition(remembered_input,"contents",connection,head,6,selected,7,16);});
  throws<std::invalid_argument>([&]{(void)host.restore_temporary_cognition(remembered_input,"contents",connection,{},7,selected,7,16);});
  CHECK(host.session().find(connection)->snapshot().record==before.record);
 }
 {
  const auto path=root/"scope-main-live-restore";fs::create_directory(path);
  ExperienceLocation input,selected,remembered,observation_head;DigestBytes connection;
  const auto observe=[&](Runtime& host,unsigned n,EvidenceOutcome outcome){
   const auto parent=host.retain({n,n,"main-restore","user","text/plain",content},7,n).original;
   EvidenceObservation value;value.source=id(n+20);value.producer=id(n+60);value.observed_at=n;value.outcome=outcome;
   auto result=host.observe_input_scope(parent,"contents",{n,n,"main-restore","tool","text/plain",content},value,7,n);
   connection=result.refinement.connection();return result.original;
  };
  {
   auto host=Runtime::create(path,config,memory);host.start_session(id(246),"main-restore");
   for(unsigned n=1;n<=8;++n)selected=observe(host,n,EvidenceOutcome::support);
   host.end_session();CHECK(host.work(7,8)==1);
   host.start_session(id(247),"main-restore");
   auto received=host.receive({9,9,"main-restore","user","text/plain",content},7,9);input=received.recorded.original;
   auto scoped=host.input_scope(received.recalled,"contents");CHECK(!scoped.temporary());
   auto cognition=host.cognize(scoped,7,9);CHECK(cognition&&cognition->replayed.location()==selected);
   remembered=cognition->assessment().remembered_head().record;observation_head=cognition->replayed.observation_head();
   CHECK(observation_head==ExperienceLocation{});
   // A different ended session advances the Main while this session remains active.
   host.attach_session(id(248),"main-restore");host.select_session(id(248));
   for(unsigned n=10;n<=17;++n)(void)observe(host,n,EvidenceOutcome::refute);
   host.end_session();CHECK(host.work(7,17)==1);host.select_session(id(247));
   const auto head_before=host.main().head();
   auto live=host.compare_replay(cognition->replayed,7,17);
   CHECK(live.evidence().current_originals().size()==8);
   CHECK(live.evidence().verification().result().verification().judgment().status()==EvidenceStatus::reject);
   CHECK(requires_re_evidence(live.agreement()));
   auto reverified=host.re_evidence(cognition->replayed,live,7,17);
   CHECK(reverified.current_originals().size()==8&&host.main().head()==head_before);
  }
  auto host=Runtime::open(path,config,memory);host.resume_session(id(247));
  auto restored=host.restore_main_cognition(input,"contents",connection,remembered,observation_head,7,selected,7,18);
  CHECK(restored.replayed.location()==selected&&restored.replayed.source_identity()==id(246));
  CHECK(restored.assessment().remembered_head().observations==8&&restored.assessment().current_originals().size()==8);
  CHECK(restored.reverified&&restored.comparison.observation_boundary()==0);
  for(unsigned n=19;n<=26;++n)(void)observe(host,n,EvidenceOutcome::refute);
  const auto main_head=host.main().head();const auto local_head=host.session().find(connection)->head();
  auto updated=host.restore_main_cognition(input,"contents",connection,remembered,observation_head,7,selected,7,26);
  CHECK(updated.reverified&&updated.assessment().current_originals().size()==8);
  CHECK(updated.assessment().verification().result().verification().judgment().status()==EvidenceStatus::reject);
  CHECK(host.main().head()==main_head&&host.session().find(connection)->head()==local_head);
  throws<std::invalid_argument>([&]{(void)host.restore_main_cognition(input,"contents",connection,remembered,{},8,selected,7,26);});
  throws<std::invalid_argument>([&]{(void)host.restore_main_cognition(input,"other",connection,remembered,{},7,selected,7,26);});
  throws<std::invalid_argument>([&]{(void)host.restore_main_cognition(input,"contents",connection,{}, {},7,selected,7,26);});
  // An authenticated nonzero local boundary excludes its old observations.
  auto boundary=host.restore_main_cognition(input,"contents",connection,remembered,local_head,7,selected,7,26);
  CHECK(boundary.comparison.observation_boundary()==8&&boundary.assessment().current_originals().empty());
 }
 for(bool merged:{false,true}){
  const auto path=root/(merged?"position-main":"position-temporary");fs::create_directory(path);
  const auto consumer=merged?id(82):id(81);
  const std::string followup="continue the previous result",extra="unselected newer event";
  ExperienceLocation chosen;ReplayPosition saved;
  {
   auto host=Runtime::create(path,config,memory);host.start_session(id(81),"position-source");
   auto input=host.receive({0,0,"position-source","user","text/plain",content},7,0).recorded.original;
   EvidenceObservation value;value.source=id(83);value.producer=id(84);value.observed_at=1;value.outcome=EvidenceOutcome::refute;
   chosen=host.observe_input_scope(input,"result",{1,1,"position-source","tool","text/plain",content},value,7,1).original;
   if(merged){host.end_session();CHECK(host.work(7,1)==1);host.start_session(consumer,"position-consumer");}
   auto parent=host.replay(host.input("text/plain",content),0);
   auto related=host.related(parent);auto cognition=host.cognize(related,7,1);
   CHECK(cognition&&cognition->replayed.location()==chosen);
   saved=*host.session().read_replay_position();CHECK(saved.original==chosen&&saved.source==id(81));
   const auto w=writes;const auto bytes=host.storage().used();
   (void)host.cognize(related,7,1);CHECK(writes==w&&host.storage().used()==bytes);
   const auto r=reads;
   auto continued=host.input("text/plain",std::as_bytes(std::span(followup)));
   CHECK(reads==r&&writes==w);
   CHECK(continued.matches()[*host.select_replay(continued)].original==chosen);
   // Fail publishing a different completed Replay. The durable old cursor
   // survives; this owner must not continue after a failed metadata write.
   const auto name=merged?"position-consumer":"position-source";
   (void)host.retain({2,2,name,"tool","text/plain",std::as_bytes(std::span(extra))},7,2);
   auto newer=host.input("text/plain",std::as_bytes(std::span(extra)));
   fail_write=true;throws<std::system_error>([&]{(void)host.replay(newer,0);});
   CHECK(!fail_write&&!host.session().usable());
  }
  {
   auto host=Runtime::open(path,config,memory);host.resume_session(consumer);
   CHECK(host.session().read_replay_position()==saved);
   const auto r=reads,w=writes;
   auto continued=host.input("text/plain",std::as_bytes(std::span(followup)));
   CHECK(reads==r&&writes==w);
   CHECK(continued.matches()[*host.select_replay(continued)].original==chosen);
   CHECK(host.work(7,2)==0); // Reopening did not end or merge the consumer.
  }
  // A valid derived record is insufficient: the referenced source and index
  // must belong to the verified connection. Preserve originals during probes.
  for(unsigned attack=0;attack<2;++attack){
   {auto store=SessionStore::open(path,consumer,memory);auto wrong=saved;
    if(attack==0)wrong.source=id(250);else wrong.original_index=UINT64_MAX;
    store.save_replay_position(wrong);}
   {auto host=Runtime::open(path,config,memory);
    throws<std::invalid_argument>([&]{host.resume_session(consumer);});CHECK(host.attached_sessions()==0);}
   {auto store=SessionStore::open(path,consumer,memory);store.save_replay_position(saved);}
  }
  {auto host=Runtime::open(path,config,memory);host.resume_session(consumer);
   CHECK(host.session().read_replay_position()==saved);}
 }
 for(bool main:{false,true}){
  for(unsigned route=0;route<3;++route){
   // Context seed is session-local; Main is tested through exact/continuation.
   if(main&&route==2)continue;
   const auto path=root/("general-restore-"+std::to_string(main)+"-"+std::to_string(route));fs::create_directory(path);
   ReplayRecovery saved;ExperienceLocation input;DigestBytes claim;
   const std::string new_text="a new utterance following the selected memory";
   const auto new_bytes=std::as_bytes(std::span(new_text));
   auto observe=[&](Runtime& host,unsigned n,EvidenceOutcome outcome){
    const auto parent=host.receive({2*n,2*n,"general","user","text/plain",content},7,2*n).recorded.original;
    EvidenceObservation value;value.source=id(n+20);value.producer=id(n+60);value.observed_at=2*n+1;value.outcome=outcome;
    const auto recorded=host.observe_input(parent,{2*n+1,2*n+1,"general","tool","text/plain",content},value,7,2*n+1);
    claim=recorded.refinement.connection();
   };
   {
    auto host=Runtime::create(path,config,memory);host.start_session(id(230),"general");
    for(unsigned n=1;n<=8;++n)observe(host,n,EvidenceOutcome::support);
    if(main){host.end_session();CHECK(host.work(7,18)==1);host.start_session(id(231),"general");}
    if(route==1){const auto recalled=host.input("text/plain",content);CHECK(host.cognize(recalled,7,18).has_value());}
    auto received=host.receive({20,20,"general","user","text/plain",route?new_bytes:content},7,20);
    input=received.recorded.original;
    auto cognition=host.cognize(received.recalled,7,20);CHECK(cognition.has_value());
    saved.temporary=received.recalled.temporary();saved.seed_only=received.recalled.seed_only();
    saved.key_kind=received.recalled.key_kind();saved.lookup_key=received.recalled.lookup_key();
    CHECK(saved.key_kind==(route==0?FamiliarityKey::exact:route==1?FamiliarityKey::continuation:FamiliarityKey::context));
    CHECK(saved.temporary!=main);
    saved.input_cue=cognition->replayed.input_cue();saved.connection=cognition->assessment().remembered_head().identity;
    saved.source=cognition->replayed.source_identity();saved.original=cognition->replayed.location();
    saved.original_index=cognition->replayed.original_index();saved.remembered_head=cognition->assessment().remembered_head().record;
    saved.observation_head=cognition->replayed.observation_head();saved.observation_boundary=cognition->comparison.observation_boundary();
   }
   auto host=Runtime::open(path,config,memory);host.resume_session(main?id(231):id(230));
   auto restored=host.restore_cognition(input,saved,7,20);
   CHECK(restored.replayed.location()==saved.original&&restored.replayed.source_identity()==saved.source);
   CHECK(restored.comparison.observation_boundary()==saved.observation_boundary);
   CHECK(!restored.reverified);
   for(unsigned n=11;n<=18;++n)observe(host,n,EvidenceOutcome::refute);
   const auto head=host.session().find(claim)->head();const auto main_head=host.main().head();
   auto updated=host.restore_cognition(input,saved,7,37);
   CHECK(updated.replayed.location()==saved.original&&updated.comparison.observation_boundary()==saved.observation_boundary);
   if(route!=2)CHECK(updated.reverified.has_value()&&updated.assessment().agreement()==ReplayAgreement::contradicts);
   CHECK(host.session().find(claim)->head()==head&&host.main().head()==main_head);
   auto bad=saved;bad.input_cue=id(252);throws<std::invalid_argument>([&]{(void)host.restore_cognition(input,bad,7,37);});
   bad=saved;bad.source=id(252);throws<std::invalid_argument>([&]{(void)host.restore_cognition(input,bad,7,37);});
   bad=saved;bad.lookup_key=id(252);throws<std::invalid_argument>([&]{(void)host.restore_cognition(input,bad,7,37);});
   bad=saved;bad.observation_boundary++;throws<std::invalid_argument>([&]{(void)host.restore_cognition(input,bad,7,37);});
   bad=saved;bad.original_index++;throws<std::invalid_argument>([&]{(void)host.restore_cognition(input,bad,7,37);});
   bad=saved;bad.remembered_head={};throws<std::invalid_argument>([&]{(void)host.restore_cognition(input,bad,7,37);});
   bad=saved;bad.key_kind=FamiliarityKey::missing;throws<std::invalid_argument>([&]{(void)host.restore_cognition(input,bad,7,37);});
  }
 }
 {
  const auto path=root/"portal-orphans";fs::create_directory(path);ExperienceLocation original;
  {auto host=Runtime::create(path,config,memory);host.start_session(id(225),"portal-original");
   original=host.receive({0,0,"portal-original","user","text/plain",content},7,0).recorded.original;}
  const auto directory=path/"portal-pages";fs::create_directory(directory);
  const auto filename=[&](char c){return directory/(std::string(64,c)+".block");};
  const auto page_id=[](unsigned n){DigestBytes d{};d.fill(std::byte(n));return d;};
  const std::array ranges{PortalRange{id(3),0,2},PortalRange{id(4),5,9}};
  const auto child=::fork();CHECK(child>=0);
  if(child==0){try{
   auto orphan=PortalPage::create(filename('a'),page_id(0xaa),PortalPage::Kind::cue,id(2),ranges,memory);
   auto linked=PortalPage::create(filename('b'),page_id(0xbb),PortalPage::Kind::context,id(2),ranges,memory);
   if(::link(filename('b').c_str(),(directory/"retained-link").c_str())<0)::_exit(2);
   auto corrupt=PortalPage::create(filename('d'),page_id(0xdd),PortalPage::Kind::cue,id(2),ranges,memory);
   const auto fd=::open(filename('d').c_str(),O_WRONLY);if(fd<0)::_exit(3);
   const char bad=127;if(::pwrite(fd,&bad,1,ExperienceBlock::header_bytes+120)!=1)::_exit(4);::close(fd);
   ::_exit(0);
  }catch(...){::_exit(5);}}
  int status=0;CHECK(::waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0);
  const auto orphan_bytes=fs::file_size(filename('a'));
  {auto ordinary=ExperienceBlock::create(filename('e'),page_id(0xee),4096);
   (void)ordinary.append({0,0,"ordinary","source","text/plain",content});}
  {const int fd=::open(filename('f').c_str(),O_CREAT|O_EXCL|O_WRONLY,0600);CHECK(fd>=0);
   CHECK(::write(fd,"x",1)==1);::close(fd);}
  std::optional<PortalPage> live;
  live.emplace(PortalPage::create(filename('c'),page_id(0xcc),PortalPage::Kind::context,id(2),ranges,memory));
  const auto before=stored_bytes(path,memory);auto cfg=config;cfg.storage_bytes=before-orphan_bytes;
  {
   auto host=Runtime::open(path,cfg,memory);host.resume_session(id(225));
   CHECK(!fs::exists(filename('a'))&&fs::exists(filename('b'))&&fs::exists(filename('c')));
   CHECK(fs::exists(filename('d'))&&fs::exists(filename('e'))&&fs::exists(filename('f')));
   CHECK(host.storage().used()==before-orphan_bytes);
   CHECK(host.session().read_original(original).location()==original);
   CHECK(live->load(memory).size()==ranges.size());
  }
  live.reset();CHECK(!fs::exists(filename('c')));
  auto too_small=config;too_small.storage_bytes=stored_bytes(path,memory)-1;
  throws<StorageLimit>([&]{(void)Runtime::open(path,too_small,memory);});
  CHECK(fs::exists(filename('b'))&&fs::exists(filename('d'))&&fs::exists(filename('e'))&&fs::exists(filename('f')));
 }
 CHECK(memory.used()==0);fs::remove_all(root);std::printf("runtime lifecycle tests: %u checks passed\n",checks);
}
