#include "transport/agent_event.hpp"
#include "vrs/memory_budget.hpp"
#include <cstdio>
#include <cstdlib>
using namespace swegca::transport;
using namespace swegca::architecture::kernel;
static unsigned checks=0;
#define CHECK(x) do{++checks;if(!(x)){std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);std::abort();}}while(false)
template<class F>void rejects(F f){bool failed=false;try{f();}catch(const std::exception&){failed=true;}CHECK(failed);}
int main(){
 swegca::vrs::MemoryBudget memory(1<<20);
 for(unsigned phase=0;phase<6;++phase)for(unsigned kind=0;kind<6;++kind){
  const auto actual=route_agent_event(static_cast<SessionPhase>(phase),static_cast<AgentEventKind>(kind));
  const auto expected=phase!=1||kind>3?AgentEventRoute::invalid:
      kind==0?AgentEventRoute::recall_then_record:kind==3?AgentEventRoute::end:AgentEventRoute::record;
  CHECK(actual==expected);
 }
 for(unsigned phase=0;phase<6;++phase){
  const auto p=static_cast<SessionPhase>(phase);
  CHECK(route_agent_delivery(p,true,true,true,5,2)==(phase==1?AgentDeliveryRoute::reuse:AgentDeliveryRoute::reject));
  CHECK(route_agent_delivery(p,true,false,true,5,2)==AgentDeliveryRoute::reject);
  CHECK(route_agent_delivery(p,false,false,false,0,0)==(phase==1?AgentDeliveryRoute::append:AgentDeliveryRoute::reject));
  CHECK(route_agent_delivery(p,false,false,false,0,1)==AgentDeliveryRoute::reject);
  CHECK(route_agent_delivery(p,false,false,true,5,6)==(phase==1?AgentDeliveryRoute::append:AgentDeliveryRoute::reject));
  CHECK(route_agent_delivery(p,false,false,true,5,7)==AgentDeliveryRoute::reject);
  CHECK(route_agent_delivery(p,false,false,true,5,3)==AgentDeliveryRoute::reject);
  CHECK(route_agent_delivery(p,false,false,true,UINT64_MAX,0)==AgentDeliveryRoute::reject);
 }
 CHECK(agent_delivery_identity(0,1,"same")==agent_delivery_identity(0,1,"same"));
 CHECK(agent_delivery_identity(0,1,"same")!=agent_delivery_identity(1,1,"same"));
 CHECK(agent_delivery_identity(0,1,"same")!=agent_delivery_identity(0,2,"same"));
 CHECK(agent_delivery_identity(0,1,"same")!=agent_delivery_identity(0,1,"changed"));
 {
  const std::string raw=" {\"session_id\":\"s-1\",\"hook_event_name\":\"UserPromptSubmit\",\"prompt\":\"한글\\n\\u0000🙂\",\"unknown\":{\"preserve\":42}} ";
  auto event=adapt_codex_hook(raw,memory);
  CHECK(event.native_bytes()==raw && event.session()=="s-1");
  CHECK(event.kind()==AgentEventKind::input && event.prompt().has_value());
  CHECK(event.prompt()->find('\0')!=std::string_view::npos);
  CHECK(event.fields().find("unknown")!=nullptr);
  auto moved=std::move(event);CHECK(moved.native_bytes()==raw && moved.prompt().has_value());
 }
 CHECK(memory.used()==0);
 // Pure text is agent-neutral without allocating another cue or normalizing
 // bytes. Unknown item fields and additional input items must not collapse.
 for(const auto literal:{R"("")",R"("한글\n\u0000🙂")",R"("  keep spaces  ")"}){
  const std::string raw=std::string(R"({"id":1,"method":"turn/start","params":{"threadId":"t","input":[{"type":"text","text":)")+literal+"}]}}";
  const std::string hook=std::string(R"({"session_id":"s","hook_event_name":"UserPromptSubmit","prompt":)")+literal+"}";
  auto event=adapt_codex_app_server(raw,memory);auto prompt=adapt_codex_hook(hook,memory);
  const auto reserved=memory.limit()-memory.used();auto* held=memory.allocate(reserved);
  CHECK(event.cue_media()==prompt.cue_media()&&event.cue_content()==prompt.cue_content());
  CHECK(event.native_bytes()==raw);
  auto moved=std::move(event);
  CHECK(moved.cue_media()=="text/plain"&&moved.cue_content()==prompt.cue_content());
  memory.deallocate(held,reserved);
 }
 for(const auto input:{R"([{"type":"text","text":"x","extra":true}])",
     R"([{"type":"text","text":"x"},{"type":"text","text":""}])",
     R"([{"type":"image","url":"never-fetch://asset"}])",R"([])"}){
  const std::string raw=std::string(R"({"id":1,"method":"turn/start","params":{"threadId":"t","input":)")+input+"}}";
  auto event=adapt_codex_app_server(raw,memory);
  CHECK(event.cue_media()=="application/vnd.swegca.codex-input-v1");
  CHECK(event.cue_content()==input&&event.native_bytes()==raw);
 }
 CHECK(memory.used()==0);
 for(const auto name:{"SessionStart","SessionEnd","Stop","Interrupt","PreCompact","PostCompact","SubagentStart","SubagentStop","PostToolUse","NewFutureEvent","explicit_end"}){
  const std::string raw="{\"session_id\":\"parent\",\"hook_event_name\":\""+std::string(name)+"\",\"reason\":\"other\",\"agent_id\":\"child\",\"transcript_path\":\"/never/read\"}";
  auto event=adapt_codex_hook(raw,memory);
  CHECK(route_agent_event(SessionPhase::active,event.kind())==AgentEventRoute::record);
  CHECK(!event.prompt() && event.native_bytes()==raw);
  CHECK(event.session()=="parent" && event.fields().at("agent_id").string()=="child");
 }
 for(const auto bad:{"[]","{}","{\"session_id\":\"\",\"hook_event_name\":\"Stop\"}",
  "{\"session_id\":\"s\",\"hook_event_name\":\"UserPromptSubmit\"}",
  "{\"session_id\":\"s\",\"hook_event_name\":\"UserPromptSubmit\",\"prompt\":7}"})
  rejects([&]{(void)adapt_codex_hook(bad,memory);});
 CHECK(memory.used()==0);
 const auto identity=agent_session_identity("codex","desktop-one","s");
 CHECK(identity==agent_session_identity("codex","desktop-one","s"));
 CHECK(identity!=agent_session_identity("another-agent","desktop-one","s"));
 CHECK(identity!=agent_session_identity("codex","desktop-two","s"));
 CHECK(agent_session_identity("a","bc","d")!=agent_session_identity("ab","c","d"));
 rejects([&]{(void)agent_session_identity("codex","","s");});
 {swegca::vrs::MemoryBudget tiny(1);rejects([&]{(void)adapt_codex_hook("{}",tiny);});CHECK(tiny.used()==0);}
 {
  const std::string raw=R"({"id":1,"method":"turn/start","params":{"threadId":"t","input":[{"type":"text","text":"한글"},{"type":"image","url":"never-fetch://asset"}],"unknown":42}})";
  auto event=adapt_codex_app_server(raw,memory);
  CHECK(event.native_bytes()==raw && event.session()=="t" && event.native_name()=="turn/start");
  CHECK(event.kind()==AgentEventKind::input && !event.prompt());
  CHECK(event.cue_media()=="application/vnd.swegca.codex-input-v1");
  // Syntax adaptation already validated input. An unused cue owns no encoded
  // copy; allocation failure at first consumption must leave a retry possible.
  const auto reserved=memory.limit()-memory.used();auto* held=memory.allocate(reserved);
  bool exhausted=false;try{(void)event.cue_content();}catch(const std::bad_alloc&){exhausted=true;}
  CHECK(exhausted&&event.native_bytes()==raw&&event.session()=="t");
  memory.deallocate(held,reserved);
  const auto expected=encode_json(event.fields().at("params").at("input"),memory);
  CHECK(event.cue_content()==expected);
  const auto used=memory.used();const auto* data=event.cue_content().data();
  CHECK(event.cue_content().data()==data&&memory.used()==used);
  auto input=parse_json(event.cue_content(),memory);
  CHECK(input.values.size()==2 && input.values[1].at("url").string()=="never-fetch://asset");
  auto moved=std::move(event);CHECK(moved.native_bytes()==raw && moved.cue_content().size()>0);
 }
 CHECK(memory.used()==0);
 {
  auto event=adapt_codex_app_server(R"({"id":1,"method":"turn/start","params":{"threadId":"t","input":[]}})",memory);
  auto moved=std::move(event);CHECK(moved.cue_content()=="[]");
 }
 for(const auto method:{"item/started","item/agentMessage/delta","item/completed","turn/completed","thread/closed","thread/archived","future/event"}){
  const std::string raw="{\"method\":\""+std::string(method)+"\",\"params\":{\"threadId\":\"t\",\"unknown\":true}}";
  auto event=adapt_codex_app_server(raw,memory);
  CHECK(event.native_bytes()==raw && event.kind()==AgentEventKind::content);
  CHECK(route_agent_event(SessionPhase::active,event.kind())==AgentEventRoute::record);
  rejects([&]{(void)event.cue_content();});
 }
 for(const auto raw:{R"({"id":1,"result":{}})",
  R"({"id":1,"method":"turn/start","params":{"threadId":"t","input":"bad"}})",
  R"({"id":1,"method":"turn/start","params":{"threadId":"t","input":[{"type":"text","text":42}]}})",
  R"({"method":"turn/start","params":{"threadId":"t","input":[]}})",
  R"({"id":1,"method":"turn/steer","params":{"threadId":"t","input":[]}})",
  R"({"method":"item/completed","params":{"threadId":""}})"})
  rejects([&]{(void)adapt_codex_app_server(raw,memory);});
 CHECK(memory.used()==0);
 {
  const std::string raw=R"({"method":"thread/started","params":{"thread":{"id":"new","future":true}}})";
  auto event=adapt_codex_app_server(raw,memory);
  CHECK(event.session()=="new"&&event.native_name()=="thread/started"&&event.native_bytes()==raw);
  CHECK(event.kind()==AgentEventKind::lifecycle&&route_agent_event(SessionPhase::active,event.kind())==AgentEventRoute::record);
  CHECK(route_agent_event(SessionPhase::ended,event.kind())==AgentEventRoute::invalid);
 }
 for(const auto raw:{R"({"method":"thread/started","params":{"thread":{"id":""}}})",
  R"({"method":"thread/started","params":{"thread":{"id":"new"},"threadId":"other"}})",
  R"({"id":1,"method":"thread/started","params":{"thread":{"id":"new"}}})"})
  rejects([&]{(void)adapt_codex_app_server(raw,memory);});
 CHECK(memory.used()==0);
 for(const auto raw:{R"({"id":1,"method":"initialize","params":{"clientInfo":{"name":"fixture"}}})",
     R"({"method":"initialized"})",R"({"method":"notice","params":null})",
     R"({"id":"start","method":"thread/start","params":{"cwd":"/fixture"}})"}){
  auto event=adapt_codex_app_server_connection(raw,"connection-1",memory);
  CHECK(event.session()=="connection-1"&&event.native_bytes()==raw&&!event.native_name().empty());
  CHECK(event.is_app_server()&&event.kind()==AgentEventKind::content);
  CHECK(route_agent_event(SessionPhase::active,event.kind())==AgentEventRoute::record);
  CHECK(!event.fields().find("threadId"));
 }
 for(const auto raw:{R"({"id":1,"method":"turn/start","params":{"input":[]}})",
     R"({"method":"turn/steer"})",R"({"method":"thread/started"})",
     R"({"method":"notice","params":{"threadId":"t"}})",
     R"({"method":"notice","params":{"threadId":null}})",
     R"({"method":"notice","params":[]})",R"({"method":""})",
     R"({"id":1,"result":null})",R"({"method":"notice","result":null})"})
  rejects([&]{(void)adapt_codex_app_server_connection(raw,"connection-1",memory);});
 rejects([&]{(void)adapt_codex_app_server_connection(R"({"method":"initialized"})","",memory);});
 CHECK(memory.used()==0);
 {
  auto event=adapt_codex_app_server(R"({"id":91,"method":"thread/resume","params":{"threadId":"existing"}})",memory);
  CHECK(event.session()=="existing"&&event.kind()==AgentEventKind::lifecycle);
  CHECK(route_agent_event(SessionPhase::active,event.kind())==AgentEventRoute::record);
 }
 for(const auto raw:{R"({"method":"thread/resume","params":{"threadId":"existing"}})",
     R"({"id":1,"method":"thread/resume","params":{}})"}){
  rejects([&]{(void)adapt_codex_app_server(raw,memory);});
  rejects([&]{(void)adapt_codex_app_server_connection(raw,"connection",memory);});
 }
 CHECK(memory.used()==0);
 // Owned frames preserve byte identity and the borrowed adapter's semantics.
 for(const bool connection:{false,true}){
  const std::string raw=connection?
   R"( {"id":1,"method":"initialize","params":{"unknown":"keep"}} )":
   R"( {"id":1,"method":"turn/start","params":{"threadId":"t","input":[{"type":"text","text":"hello"}],"unknown":"keep"}} )";
  {
   std::pmr::string frame(raw,&memory);const auto* address=frame.data();
   auto event=adapt_owned_codex_app_server(std::move(frame),memory,connection?"peer":"");
   auto borrowed=connection?adapt_codex_app_server_connection(raw,"peer",memory):adapt_codex_app_server(raw,memory);
   CHECK(event.native_bytes().data()==address);
   CHECK(event.native_bytes()==raw&&event.session()==borrowed.session());
   CHECK(event.kind()==borrowed.kind());
   if(!connection)CHECK(event.cue_content()==borrowed.cue_content());
   auto moved=std::move(event);
   CHECK(moved.native_bytes().data()==address&&moved.native_bytes()==raw);
   CHECK(moved.fields().at("params").at("unknown").string()=="keep");
  }
  CHECK(memory.used()==0);
 }
 {
  swegca::vrs::MemoryBudget other(1<<20);
  rejects([&]{(void)adapt_owned_codex_app_server(std::pmr::string("{}", &other),memory);});
  CHECK(other.used()==0&&memory.used()==0);
 }
 for(const auto raw:{"{",R"({"id":1,"method":"turn/start","params":{"threadId":"t","input":3}})"}){
  rejects([&]{(void)adapt_owned_codex_app_server(std::pmr::string(raw,&memory),memory);});
  CHECK(memory.used()==0);
 }
 {
  const std::string raw=R"({"id":1,"method":"turn/start","params":{"threadId":"t","input":[{"type":"text","text":")"+
      std::string(1<<20,'x')+R"("}]}})";
  swegca::vrs::MemoryBudget borrowed_memory(8<<20),owned_memory(8<<20);
  std::size_t borrowed_used=0,owned_used=0;
  {
   std::pmr::string frame(raw,&borrowed_memory);
   auto event=adapt_codex_app_server(frame,borrowed_memory);
   CHECK(event.native_bytes()==raw);
   borrowed_used=borrowed_memory.used();
  }
  {
   std::pmr::string frame(raw,&owned_memory);const auto* address=frame.data();
   auto event=adapt_owned_codex_app_server(std::move(frame),owned_memory);
   CHECK(event.native_bytes()==raw&&event.native_bytes().data()==address);
   owned_used=owned_memory.used();
  }
  CHECK(borrowed_memory.used()==0&&owned_memory.used()==0);
  CHECK(borrowed_used>=owned_used+raw.size());
  CHECK(owned_memory.peak_reserved()<=borrowed_memory.peak_reserved());
 }
 std::printf("agent event tests: %u checks passed\n",checks);
}
