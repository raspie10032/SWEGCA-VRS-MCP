#include "transport/json.hpp"
#include "vrs/memory_budget.hpp"
#include <sstream>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>
using namespace swegca::transport;
unsigned checks=0;
#define CHECK(x) do{++checks;if(!(x)){std::fprintf(stderr,"FAIL %d\n",__LINE__);std::abort();}}while(false)
int main(){
 swegca::vrs::MemoryBudget memory(4<<20);
 std::string controls;for(char c=0;c<32;++c)controls+=c;
 for(const auto& text:{std::string{},controls,std::string("한글🙂 quote\" slash\\ tail"),std::string(1<<20,'a')}){
  const auto expected=quote_json(text,memory);
  CHECK(parse_json(expected,memory).string()==text);
  const auto used=memory.used();std::ostringstream out;
  write_json_string(out,text);CHECK(std::string_view(out.str())==std::string_view(expected));CHECK(memory.used()==used);
 }
 CHECK(memory.used()==0);
 // Exercise every ASCII byte on both sides of all 16-byte lane boundaries.
 for(unsigned offset=0;offset<33;++offset)for(unsigned c=0;c<128;++c){
  const std::string text=std::string(offset,'a')+char(c)+std::string(35,'z');
  std::string escaped;const char digits[]="0123456789abcdef";
  if(c=='"'||c=='\\'){escaped+='\\';escaped+=char(c);}
  else if(c<32){escaped="\\u00";escaped+=digits[c>>4];escaped+=digits[c&15];}
  else escaped+=char(c);
  const auto expected="\""+std::string(offset,'a')+escaped+std::string(35,'z')+"\"";
  CHECK(std::string_view(quote_json(text,memory))==expected);
  CHECK(parse_json(expected,memory).string()==text);
  std::ostringstream encoded;write_json_string(encoded,text);CHECK(encoded.str()==expected);
 }
 for(unsigned offset=0;offset<33;++offset){
  for(const auto unicode:{"한글🙂","\xc2\x80","\xf4\x8f\xbf\xbf"}){
   const auto text=std::string(offset,'a')+unicode+std::string(35,'z');
   CHECK(parse_json(quote_json(text,memory),memory).string()==text);
  }
  for(const auto invalid:{"\x80","\xc0\x80","\xed\xa0\x80","\xf4\x90\x80\x80","\xf0\x90"}){
   const auto text=std::string(offset,'a')+invalid;
   std::ostringstream encoded;bool rejected=false;
   try{write_json_string(encoded,text);}catch(const std::invalid_argument&){rejected=true;}
   CHECK(rejected&&encoded.str().empty());
   rejected=false;try{(void)parse_json("\""+text+"\"",memory);}catch(const std::invalid_argument&){rejected=true;}CHECK(rejected);
  }
 }
 // A full-width load must never cross the supplied range into an unreadable
 // page, including views with no accessible terminating byte.
 const auto page=static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
 auto* region=static_cast<char*>(::mmap(nullptr,page*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
 CHECK(region!=MAP_FAILED);CHECK(::mprotect(region+page,page,PROT_NONE)==0);
 for(unsigned size=0;size<65;++size){
  auto* start=region+page-size;std::memset(start,'a',size);
  const std::string_view text(start,size);
  CHECK(std::string_view(quote_json(text,memory))=="\""+std::string(size,'a')+"\"");
  if(size>=2){start[0]='"';start[size-1]='"';CHECK(parse_json(text,memory).string()==std::string(size-2,'a'));}
 }
 CHECK(::munmap(region,page*2)==0);
 for(const auto raw:{"\"plain\\n끝\\uD83D\\uDE42tail\"","\"\\\"\\\\\\/\\b\\f\\n\\r\\t\""}){
  const auto parsed=parse_json(raw,memory);
  CHECK(parse_json(encode_json(parsed,memory),memory).string()==parsed.string());
 }
 for(const auto raw:{"\"unterminated","\"raw\ncontrol\"","\"bad\\q\"","\"\\uD800\"","\"\\uDC00\"","\"\\uD800\\u0000\"","\"tail\\"}){
  bool rejected=false;try{(void)parse_json(raw,memory);}catch(const std::invalid_argument&){rejected=true;}
  CHECK(rejected);
 }
 for(const auto& invalid:{std::string("\xc0\x80",2),std::string("\xed\xa0\x80",3),std::string("\xf0\x90",2)}){
  std::ostringstream out;bool failed=false;
  try{write_json_string(out,invalid);}catch(const std::invalid_argument&){failed=true;}
  CHECK(failed&&out.str().empty());
 }
 // Caller has no remaining VRS allocation budget: quoting to an existing
 // stream still works, while constructing an escaped PMR copy cannot.
 auto* held=memory.allocate(memory.limit());const std::string body(65536,'x');
 bool failed=false;try{(void)quote_json(body,memory);}catch(const std::bad_alloc&){failed=true;}
 CHECK(failed);std::ostringstream out;write_json_string(out,body);CHECK(out.str().size()==body.size()+2);
 memory.deallocate(held,memory.limit());CHECK(memory.used()==0);
 for(const auto size:{0U,1U,2048U,2049U,8193U}){
  std::vector<std::byte> bytes(size);for(unsigned i=0;i<size;++i)bytes[i]=std::byte(i%256);
  std::ostringstream encoded;write_json_hex(encoded,bytes);const auto text=encoded.str();
  CHECK(text.size()==size*2);
  const auto digit=[](char c){return c<='9'?c-'0':c-'a'+10;};
  bool valid=true;for(unsigned i=0;i<size;++i)valid&=unsigned(digit(text[2*i])*16+digit(text[2*i+1]))==i%256;
  CHECK(valid);
 }
 std::printf("JSON stream tests: %u checks passed\n",checks);
}
