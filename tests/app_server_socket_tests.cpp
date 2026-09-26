#include "transport/app_server_wire.hpp"
#include "vrs/memory_budget.hpp"
#include <cstdio>
#include <cstdlib>
using namespace swegca::transport;
static unsigned checks=0;static bool interrupt_send=false;
extern "C" ssize_t __real_send(int,const void*,size_t,int);
extern "C" ssize_t __wrap_send(int fd,const void* bytes,size_t count,int flags){
 if(interrupt_send){interrupt_send=false;errno=EINTR;return -1;}
 return __real_send(fd,bytes,count,flags);
}
#define CHECK(x) do{++checks;if(!(x)){std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);std::abort();}}while(false)
template<class F>void rejects(F f){bool failed=false;try{f();}catch(const std::exception&){failed=true;}CHECK(failed);}
int main(){
 swegca::vrs::MemoryBudget memory(8<<20);
 const std::string native="{\"method\":\"item/agentMessage/delta\",\"params\":{\"threadId\":\"a\",\"delta\":\""+std::string(512<<10,'x')+"\"}}";
 {
  AppServerWire wire(memory,1,1);wire.attach("a",0);
  int pair[2];CHECK(::socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,pair)==0);
  const int small=1024;CHECK(::setsockopt(pair[0],SOL_SOCKET,SO_SNDBUF,&small,sizeof(small))==0);
  auto frame=wire.prepare(native,RpcSender::server,42);
  rejects([&]{wire.bind_socket(frame,pair[0]);});
  wire.recorded(frame);wire.bind_socket(frame,pair[0]);
  rejects([&]{wire.bind_socket(frame,pair[0]);});
  // Closing the caller's fd cannot change the socket retained by the frame.
  ::close(pair[0]);
  interrupt_send=true;CHECK(!wire.send_ready(frame)&&frame.sent_bytes()==0);
  auto competing=wire.prepare(native,RpcSender::server,43);wire.recorded(competing);
  rejects([&]{wire.bind_socket(competing,pair[1]);});
  bool stalled=false;
  for(unsigned n=0;n<128;++n){const auto before=frame.sent_bytes();CHECK(!wire.send_ready(frame));if(frame.sent_bytes()==before){stalled=true;break;}}
  CHECK(stalled&&frame.sent_bytes()>0&&frame.sent_bytes()<native.size());
  const auto prefix=frame.sent_bytes();auto moved=std::move(frame);
  rejects([&]{(void)wire.send_ready(frame);});CHECK(moved.sent_bytes()==prefix);
  std::string received;char buffer[997];bool complete=false;
  for(unsigned n=0;n<100000 && !complete;++n){
   const auto count=::recv(pair[1],buffer,sizeof(buffer),MSG_DONTWAIT);
   if(count>0)received.append(buffer,static_cast<std::size_t>(count));
   else CHECK(count<0&&(errno==EAGAIN||errno==EWOULDBLOCK));
   complete=wire.send_ready(moved);
  }
  CHECK(complete&&moved.sent_bytes()==native.size()+1);
  while(true){const auto count=::recv(pair[1],buffer,sizeof(buffer),0);if(count==0)break;CHECK(count>0);received.append(buffer,static_cast<std::size_t>(count));}
  CHECK(received==native+"\n");
  CHECK(wire.send_ready(moved));CHECK(::recv(pair[1],buffer,sizeof(buffer),MSG_DONTWAIT)==0);
  rejects([&]{wire.bind_socket(moved,pair[1]);});::close(pair[1]);
 }
 CHECK(memory.used()==0);
 {
  AppServerWire wire(memory,1,1);wire.attach("a",0);
  int pair[2];CHECK(::socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,pair)==0);
  const int small=1024;CHECK(::setsockopt(pair[0],SOL_SOCKET,SO_SNDBUF,&small,sizeof(small))==0);
  auto frame=wire.prepare(native,RpcSender::server,42);wire.recorded(frame);wire.bind_socket(frame,pair[0]);
  CHECK(!wire.send_ready(frame));const auto prefix=frame.sent_bytes();CHECK(prefix>0&&prefix<native.size());
  ::close(pair[1]);rejects([&]{(void)wire.send_ready(frame);});CHECK(frame.sent_bytes()==prefix);
  rejects([&]{wire.bind_socket(frame,pair[0]);});::close(pair[0]);
 }
 CHECK(memory.used()==0);
 {
  AppServerWire wire(memory,1,1);wire.attach("a",0);
  int pair[2];CHECK(::socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,pair)==0);
  const int small=1024;CHECK(::setsockopt(pair[0],SOL_SOCKET,SO_SNDBUF,&small,sizeof(small))==0);
  {
   auto abandoned=wire.prepare(native,RpcSender::server,42);wire.recorded(abandoned);wire.bind_socket(abandoned,pair[0]);
   CHECK(!wire.send_ready(abandoned));CHECK(abandoned.sent_bytes()>0);
  }
  auto next=wire.prepare(native,RpcSender::server,43);wire.recorded(next);
  rejects([&]{wire.bind_socket(next,pair[0]);}); // Partial prefix cannot be skipped.
  ::close(pair[0]);::close(pair[1]);
 }
 CHECK(memory.used()==0);
 {
  AppServerWire wire(memory,1,1);wire.attach("a",0);
  auto frame=wire.prepare(native+"\n",RpcSender::server,42);wire.recorded(frame);
  rejects([&]{wire.bind_socket(frame,-1);});CHECK(frame.sent_bytes()==0);
 }
 CHECK(memory.used()==0);
 std::printf("app-server socket forwarding tests: %u checks passed\n",checks);
}
