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
 std::printf("agent event tests: %u checks passed\n",checks);
}
