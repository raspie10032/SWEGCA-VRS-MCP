#include "transport/socket_frames.hpp"
#include "vrs/memory_budget.hpp"
#include <cstdio>
#include <cstdlib>
using namespace swegca::transport;
static unsigned checks=0;static bool interrupt_read=false;
extern "C" ssize_t __real_recv(int,void*,size_t,int);
extern "C" ssize_t __wrap_recv(int fd,void* bytes,size_t count,int flags){
 if(interrupt_read){interrupt_read=false;errno=EINTR;return -1;}
 return __real_recv(fd,bytes,count,flags);
}
#define CHECK(x) do{++checks;if(!(x)){std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);std::abort();}}while(false)
template<class F>void rejects(F f){bool failed=false;try{f();}catch(const std::exception&){failed=true;}CHECK(failed);}
void send_all(int fd,std::string_view text){while(!text.empty()){const auto count=::send(fd,text.data(),text.size(),MSG_NOSIGNAL);CHECK(count>0);text.remove_prefix(static_cast<std::size_t>(count));}}
int main(){
 swegca::vrs::MemoryBudget memory(1<<20);
 using State=SocketFrames::State;
 {
  int pair[2];CHECK(::socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,pair)==0);
  SocketFrames reader(pair[0],8,memory);::close(pair[0]);
  CHECK(reader.poll()==State::pending);rejects([&]{(void)reader.frame();});rejects([&]{reader.consumed();});
  send_all(pair[1],"ab");interrupt_read=true;CHECK(reader.poll()==State::pending&&reader.retained_bytes()==0);
  CHECK(reader.poll()==State::pending&&reader.retained_bytes()==2);
  send_all(pair[1],"c\nx\n");CHECK(reader.poll()==State::frame&&reader.frame()=="abc");
  CHECK(reader.poll()==State::frame&&reader.frame()=="abc"); // Stable until acknowledged.
  reader.consumed();CHECK(reader.buffered());CHECK(reader.poll()==State::frame&&reader.frame()=="x");
  reader.consumed();::close(pair[1]);CHECK(reader.poll()==State::end);CHECK(reader.poll()==State::end);
 }
 CHECK(memory.used()==0);
 {
  int pair[2];CHECK(::socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,pair)==0);
  SocketFrames reader(pair[0],4,memory);::close(pair[0]);
  send_all(pair[1],"1234\n\n");CHECK(reader.poll()==State::frame&&reader.frame()=="1234");reader.consumed();
  CHECK(reader.poll()==State::frame&&reader.frame().empty());reader.consumed();
  send_all(pair[1],"12345\nvalid\n");rejects([&]{(void)reader.poll();});
  CHECK(reader.retained_bytes()>4);rejects([&]{(void)reader.frame();});rejects([&]{(void)reader.poll();});::close(pair[1]);
 }
 CHECK(memory.used()==0);
 {
  int pair[2];CHECK(::socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,pair)==0);
  SocketFrames reader(pair[0],10,memory);::close(pair[0]);send_all(pair[1],"partial");::close(pair[1]);
  CHECK(reader.poll()==State::pending);rejects([&]{(void)reader.poll();});CHECK(reader.retained_bytes()==7);
  rejects([&]{(void)reader.frame();});rejects([&]{(void)reader.poll();});
 }
 CHECK(memory.used()==0);
 {
  int pair[2];CHECK(::socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,pair)==0);
  swegca::vrs::MemoryBudget budget(32768);SocketFrames reader(pair[0],16384,budget);::close(pair[0]);
  const std::string text(9000,'z');send_all(pair[1],text+"\nnext\n");
  void* held=budget.allocate(32768);rejects([&]{(void)reader.poll();});CHECK(reader.retained_bytes()>0);
  budget.deallocate(held,32768);
  auto state=State::pending;for(unsigned n=0;n<10&&state==State::pending;++n)state=reader.poll();
  CHECK(state==State::frame&&reader.frame()==text);reader.consumed();
  CHECK(reader.poll()==State::frame&&reader.frame()=="next");::close(pair[1]);
 }
 CHECK(memory.used()==0);
 // Newline immediately before, on and after a staging boundary; preserve
 // following frames and all bytes of a multi-read message (including NUL).
 for(const std::size_t size:{65535U,65536U,65537U,131073U}){
  int pair[2];CHECK(::socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,pair)==0);
  {
   SocketFrames reader(pair[0],size,memory);::close(pair[0]);
   std::string payload(size,'q');payload[size/2]='\0';
   send_all(pair[1],payload+"\nnext\n");::close(pair[1]);
   auto state=State::pending;
   for(unsigned n=0;n<40&&state==State::pending;++n)state=reader.poll();
   CHECK(state==State::frame&&reader.frame()==payload);reader.consumed();
   state=State::pending;
   for(unsigned n=0;n<40&&state==State::pending;++n)state=reader.poll();
   CHECK(state==State::frame&&reader.frame()=="next");reader.consumed();
   CHECK(reader.poll()==State::end);
  }
  CHECK(memory.used()==0);
 }
 std::printf("socket frame reader tests: %u checks passed\n",checks);
}
