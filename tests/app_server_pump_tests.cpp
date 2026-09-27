#include "transport/app_server_pump.hpp"
#include "transport/agent_event_commit.hpp"
#include "vrs/memory_budget.hpp"
#include <cstdio>
#include <cstdlib>
#include <iostream>
using namespace swegca::transport;
static unsigned checks=0;
#define CHECK(x) do{++checks;if(!(x)){std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);std::abort();}}while(false)
template<class F>void rejects(F f){bool failed=false;try{f();}catch(const std::exception&){failed=true;}CHECK(failed);}
void send_frame(int fd,std::string_view text){auto framed=std::string(text)+"\n";std::string_view rest=framed;while(!rest.empty()){const auto n=::send(fd,rest.data(),rest.size(),MSG_NOSIGNAL);CHECK(n>0);rest.remove_prefix(static_cast<std::size_t>(n));}}
void commit_checks(swegca::vrs::MemoryBudget& memory){
 const std::string identity(64,'a');
 rejects([&]{AgentEventCommit invalid("bad",parse_json("{}",memory),"{}","x",memory);});
 rejects([&]{AgentEventCommit invalid(identity,parse_json("[]",memory),"{}","x",memory);});
 rejects([&]{AgentEventCommit invalid(identity,parse_json("{}",memory),"{}","",memory);});
 rejects([&]{AgentEventCommit invalid(identity,parse_json(R"({"identity":"other"})",memory),"{}","x",memory);});
 rejects([&]{AgentEventCommit invalid(identity,parse_json(R"({"native":"other"})",memory),"{}","x",memory);});
 rejects([&]{AgentEventCommit invalid(identity,parse_json("{}",memory),std::string("\xff",1),"x",memory);});
 rejects([&]{auto malformed=parse_json("{}",memory);malformed.keys.emplace_back("native");AgentEventCommit invalid(identity,std::move(malformed),"{}","x",memory);});
 rejects([&]{auto duplicate=parse_json(R"({"native":"a"})",memory);duplicate.keys.emplace_back("native");Json extra(&memory);extra.kind=Json::Kind::string;duplicate.values.push_back(std::move(extra));AgentEventCommit invalid(identity,std::move(duplicate),"{}","x",memory);});
 {
  std::string native(65536,'x');native+="한글🙂\"\\";for(char c=0;c<32;++c)native+=c;
  auto fields=parse_json("{}",memory);
  AgentEventCommit large(identity,std::move(fields),native,"large",memory);
  const auto decoded=parse_json(large.request(),memory);
  CHECK(decoded.at("params").at("native").string()==native);
  CHECK(decoded.at("params").at("identity").string()==identity);
  const std::string encoded(large.request());native.assign("changed");native.shrink_to_fit();
  CHECK(large.request()==encoded);
 }
 AgentEventCommit commit(identity,parse_json(R"({"sequence":"0"})",memory),"line\nnext","x",memory);
 const std::string pending(commit.request());
 CHECK(parse_json(pending,memory).at("params").at("identity").string()==identity);
 rejects([&]{(void)commit.reply();});
 for(const auto* raw:{R"({"jsonrpc":"2.0","id":"wrong","result":{}})",
     R"({"jsonrpc":"2.0","id":"x/event","error":{"code":-1}})",
     R"({"jsonrpc":"2.0","id":"x/event","result":{},"error":{}})",
     R"({"jsonrpc":"1.0","id":"x/select","result":{}})",
     R"({"jsonrpc":"2.0","id":1,"result":{}})",
     R"({"jsonrpc":"2.0","id":"x/event","result":null})",
     R"({"jsonrpc":"2.0","id":"x/select","result":{"unknown":true}})"}){
  rejects([&]{commit.accept(raw);});CHECK(commit.request()==pending);
 }
 CHECK(commit.stage()==AgentEventCommit::Stage::event);
 const std::string event(commit.request());
 CHECK(parse_json(event,memory).at("method").string()=="swegca/agent/event");
 CHECK(event.find('\n')==std::string::npos);
 CHECK(parse_json(event,memory).at("params").at("native").string()=="line\nnext");
 const auto response=[&](std::string_view bytes){return "{\"jsonrpc\":\"2.0\",\"id\":\"x/event\",\"result\":{\"original\":{\"block\":\""+identity+"\",\"digest\":\""+identity+"\",\"offset\":\"64\",\"bytes\":\""+std::string(bytes)+"\"}}}";};
 for(const auto* raw:{R"({"jsonrpc":"2.0","id":"x/select","result":{}})",
     R"({"jsonrpc":"2.0","id":"x/event","result":{}})",
     R"({"jsonrpc":"2.0","id":"x/event","result":{"original":"forged"}})",
     R"({"jsonrpc":"2.0","id":"x/event","result":{"original":{"block":"bad"}}})"}){
  rejects([&]{commit.accept(raw);});CHECK(commit.request()==event);
 }
 for(const auto* bytes:{"0","-1","18446744073709551615","18446744073709551616","1.0"}){
  rejects([&]{commit.accept(response(bytes));});CHECK(commit.request()==event);
 }
 commit.accept(response("128"));
 CHECK(commit.stage()==AgentEventCommit::Stage::complete&&commit.reply()==response("128"));
 rejects([&]{(void)commit.request();});rejects([&]{commit.accept(response("128"));});
}
int main(int argc,char** argv){
 swegca::vrs::MemoryBudget memory(8<<20);
 if(argc==1){commit_checks(memory);CHECK(memory.used()==0);}
 const std::string a=R"({"id":1,"method":"turn/start","params":{"threadId":"a","input":[{"type":"text","text":"입력"}]}})";
 const std::string b=R"({"id":1,"method":"item/commandExecution/requestApproval","params":{"threadId":"b","unknown":true}})";
 const std::string reply=R"({"id":1,"result":{"original":"reply"}})";
 using State=AppServerPump::State;
 int client[2],server[2];CHECK(::socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,client)==0);CHECK(::socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,server)==0);
 {
  AppServerPump pump(client[0],server[0],65536,memory,2,2);pump.attach("a",0);pump.attach("b",0);
  ::close(client[0]);::close(server[0]);
  SocketFrames client_out(client[1],65536,memory),server_out(server[1],65536,memory);
  if(argc==2&&std::string_view(argv[1])=="--exchange"){
   std::string identities;CHECK(bool(std::getline(std::cin,identities)));
   const auto bindings=parse_json(identities,memory);unsigned rpc_serial=0;
   const auto deliver=[&](std::string_view raw,RpcSender sender){
    send_frame(sender==RpcSender::client?client[1]:server[1],raw);
    unsigned calls=0;
    const auto ingest=[&](const AppServerWire::Delivery& plan){
     ++calls;
     AgentEventCommit commit(bindings.at(plan.event().session()).string(),pump.metadata(plan,7,0),plan.event().native_bytes(),std::to_string(++rpc_serial),memory);
     const auto request=parse_json(commit.request(),memory);
     std::cout<<"{\"session\":"<<quote_json(plan.event().session(),memory)<<",\"parameters\":"<<encode_json(request.at("params"),memory)<<"}\n"<<std::flush;
     while(commit.stage()!=AgentEventCommit::Stage::complete){
      std::cout<<commit.request()<<"\n"<<std::flush;
      std::string ack;if(!std::getline(std::cin,ack))throw std::runtime_error("ingestion not confirmed");
      commit.accept(ack);
     }
     return true;
    };
    State state=State::idle;for(unsigned n=0;n<16&&state!=State::forwarded;++n)state=pump.step(sender,42,ingest);
    CHECK(state==State::forwarded&&calls==1);
    auto& receiver=sender==RpcSender::client?server_out:client_out;
    CHECK(receiver.poll()==SocketFrames::State::frame&&receiver.frame()==raw);
    std::cout<<"{\"forwarded\":"<<quote_json(receiver.frame(),memory)<<"}\n"<<std::flush;receiver.consumed();
   };
   deliver(a,RpcSender::client);deliver(b,RpcSender::server);deliver(reply,RpcSender::server);deliver(reply,RpcSender::client);
   CHECK(pump.pending_requests()==0);
  }else{
   unsigned calls=0;std::string attempted;
   const auto rejected=[&](const AppServerWire::Delivery& plan){++calls;attempted=encode_json(pump.metadata(plan,7,0),memory);return false;};
   CHECK(pump.step(RpcSender::client,42,rejected)==State::idle&&calls==0);
   send_frame(client[1],a);send_frame(server[1],b);
   CHECK(pump.step(RpcSender::client,42,rejected)==State::waiting_for_record&&calls==1);
   CHECK(server_out.poll()==SocketFrames::State::pending);
   CHECK(pump.step(RpcSender::server,43,rejected)==State::waiting_for_record&&calls==1);
   const auto accepted=[&](const AppServerWire::Delivery& plan){++calls;CHECK(std::string_view(encode_json(pump.metadata(plan,7,0),memory))==attempted);return true;};
   CHECK(pump.step(RpcSender::client,999,accepted)==State::forwarding&&calls==2);
   CHECK(pump.step(RpcSender::client,1000,accepted)==State::forwarded&&calls==2);
   CHECK(server_out.poll()==SocketFrames::State::frame&&server_out.frame()==a);server_out.consumed();
   const auto fail=[](const AppServerWire::Delivery&)->bool{throw std::runtime_error("lost storage acknowledgement");};
   rejects([&]{(void)pump.step(RpcSender::server,45,fail);});
   CHECK(client_out.poll()==SocketFrames::State::pending);
   const auto accept_b=[&](const AppServerWire::Delivery& plan){CHECK(plan.event().session()=="b"&&plan.observed_at()==45&&plan.sequence()==0);return true;};
   CHECK(pump.step(RpcSender::server,1000,accept_b)==State::forwarding);
   CHECK(pump.step(RpcSender::server,1001,accept_b)==State::forwarded);
   CHECK(client_out.poll()==SocketFrames::State::frame&&client_out.frame()==b);client_out.consumed();
   send_frame(server[1],reply);
   const auto accept_reply=[&](const AppServerWire::Delivery& plan){CHECK(plan.event().session()=="a"&&plan.request_sequence()==0&&plan.sequence()==1);return true;};
   CHECK(pump.step(RpcSender::server,46,accept_reply)==State::forwarding);
   CHECK(pump.step(RpcSender::server,47,accept_reply)==State::forwarded);
   CHECK(client_out.poll()==SocketFrames::State::frame&&client_out.frame()==reply);client_out.consumed();
   CHECK(::shutdown(client[1],SHUT_WR)==0);CHECK(pump.step(RpcSender::client,48,rejected)==State::end);
   CHECK(pump.pending_requests()==1); // EOF neither ends VRS nor erases other requests.
  }
 }
 ::close(client[1]);::close(server[1]);CHECK(memory.used()==0);
 if(argc==1){
  int c[2],s[2];CHECK(::socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,c)==0);CHECK(::socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,s)==0);
  {
   AppServerPump pump(c[0],s[0],65536,memory,1,1);pump.attach("a",0);
   SocketFrames backend(s[1],65536,memory);
   send_frame(c[1],a);
   const std::string notification=R"({"method":"item/agentMessage/delta","params":{"threadId":"a","delta":"x"}})";
   send_frame(s[1],notification);
   unsigned calls=0;void* held=nullptr;std::size_t bytes=0;
   const auto stored=[&](const AppServerWire::Delivery& plan){
    ++calls;CHECK(plan.sequence()==0);bytes=memory.limit()-memory.used();held=memory.allocate(bytes);return true;
   };
   rejects([&]{(void)pump.step(RpcSender::client,50,stored);}); // Post-ack request registration fails.
   CHECK(calls==1&&pump.pending_requests()==0);
   CHECK(pump.step(RpcSender::server,51,stored)==State::waiting_for_record&&calls==1);
   memory.deallocate(held,bytes);
   const auto must_not_repeat=[](const AppServerWire::Delivery&)->bool{CHECK(false);return false;};
   CHECK(pump.step(RpcSender::client,52,must_not_repeat)==State::forwarding);
   CHECK(pump.step(RpcSender::client,53,must_not_repeat)==State::forwarded);
   CHECK(backend.poll()==SocketFrames::State::frame&&backend.frame()==a);backend.consumed();
   const auto following=[&](const AppServerWire::Delivery& plan){CHECK(plan.sequence()==1&&plan.event().session()=="a");return true;};
   CHECK(pump.step(RpcSender::server,54,following)==State::forwarding);
   CHECK(pump.step(RpcSender::server,55,following)==State::forwarded);
  }
  ::close(c[0]);::close(c[1]);::close(s[0]);::close(s[1]);CHECK(memory.used()==0);
 }
 if(argc==1)std::printf("app-server duplex pump tests: %u checks passed\n",checks);
}
