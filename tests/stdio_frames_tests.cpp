#include "transport/stdio_frames.hpp"
#include "vrs/memory_budget.hpp"
#include <thread>
#include <cstdio>
#include <cstdlib>
using swegca::transport::StdioFrames;
unsigned checks=0;
#define CHECK(x) do{++checks;if(!(x)){std::fprintf(stderr,"FAIL %d\n",__LINE__);std::abort();}}while(false)
struct Pipe{
 int fds[2];Pipe(){if(::pipe(fds))std::abort();}
 ~Pipe(){::close(fds[0]);if(fds[1]>=0)::close(fds[1]);}
 void write(std::string_view bytes){while(!bytes.empty()){auto n=::write(fds[1],bytes.data(),bytes.size());if(n<=0)std::abort();bytes.remove_prefix(n);}}
 void end(){::close(fds[1]);fds[1]=-1;}
};
int main(){
 swegca::vrs::MemoryBudget memory(8<<20);bool eof=false;
 {
  Pipe p;p.write("abcd\n\nabcde\nok\nlast");p.end();StdioFrames frames(p.fds[0],4,memory);
  CHECK(frames.next(eof)=="abcd"&&!eof);CHECK(frames.next(eof).empty()&&!eof);
  bool oversized=false;try{(void)frames.next(eof);}catch(const std::length_error&){oversized=true;}CHECK(oversized&&!eof);
  CHECK(frames.next(eof)=="ok"&&!eof);
  bool truncated=false;try{(void)frames.next(eof);}catch(const std::invalid_argument&){truncated=true;}CHECK(truncated&&eof);
  CHECK(frames.next(eof).empty()&&eof);
 }
 // Cross many staging boundaries, including an oversized frame followed by
 // valid data in the same stream. No whole-input allocation by the reader.
 for(const auto chunk:{1U,4095U,4096U,4097U,65536U}){
  Pipe p;const std::string payload(65536,'x');
  const auto all=payload+"\n"+payload+"y\nok\n";
  std::thread writer([&]{for(std::size_t i=0;i<all.size();i+=chunk)p.write(std::string_view(all).substr(i,chunk));p.end();});
  StdioFrames frames(p.fds[0],payload.size(),memory);
  CHECK(std::string_view(frames.next(eof))==payload&&!eof);
  bool rejected=false;try{(void)frames.next(eof);}catch(const std::length_error&){rejected=true;}CHECK(rejected&&!eof);
  CHECK(frames.next(eof)=="ok"&&!eof);CHECK(frames.next(eof).empty()&&eof);writer.join();
 }
 {
  Pipe p;p.write("12345");p.end();StdioFrames frames(p.fds[0],4,memory);
  bool rejected=false;try{(void)frames.next(eof);}catch(const std::invalid_argument&){rejected=true;}CHECK(rejected&&eof);
 }
 {
  StdioFrames frames(-1,4,memory);bool failed=false;
  try{(void)frames.next(eof);}catch(const std::system_error&){failed=true;}CHECK(failed&&!eof);
 }
 {
  Pipe p;StdioFrames frames(p.fds[0],64,memory);unsigned calls=0;
  auto idle=[&]{++calls;if(calls==1)return true;p.write("one\ntwo\n");return false;};
  CHECK(frames.next(eof,idle)=="one"&&!eof&&calls==2);
  CHECK(frames.next(eof,idle)=="two"&&!eof&&calls==2); // read-ahead takes priority
  p.end();CHECK(frames.next(eof,idle).empty()&&eof&&calls==2); // EOF is not idle work
 }
 {
  Pipe p;p.write("prefix");StdioFrames frames(p.fds[0],64,memory);unsigned calls=0;
  std::thread writer([&]{std::this_thread::sleep_for(std::chrono::milliseconds(20));p.write("tail\n");});
  CHECK(frames.next(eof,[&]{++calls;return false;})=="prefixtail"&&!eof);
  writer.join();CHECK(calls==0); // neither readable input nor a partial frame is idle
 }
 {
  StdioFrames frames(-1,4,memory);unsigned calls=0;bool failed=false;
  try{(void)frames.next(eof,[&]{++calls;return true;});}catch(const StdioFrames::ReadError&){failed=true;}
  CHECK(failed&&calls==0&&!eof);
 }
 CHECK(memory.used()==0);
 std::printf("stdio frame tests: %u checks passed\n",checks);
}
