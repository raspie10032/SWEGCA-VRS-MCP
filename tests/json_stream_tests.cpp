#include "transport/json.hpp"
#include "vrs/memory_budget.hpp"
#include <sstream>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>
#include <limits>
using namespace swegca::transport;
unsigned checks=0;
#define CHECK(x) do{++checks;if(!(x)){std::fprintf(stderr,"FAIL %d\n",__LINE__);std::abort();}}while(false)
int main(){
 swegca::vrs::MemoryBudget memory(4<<20);
 {
  constexpr std::string_view path[]{"params","native"};JsonMemberSource source;
  const std::string wire=R"( {"other":{"native":0},"params":{"native": { "unknown":"\u0041", "x":[1,true] }}} )";
  auto value=parse_json_member(wire,memory,path,source);
  CHECK(source.bytes(wire)==R"({ "unknown":"\u0041", "x":[1,true] })");
  CHECK(value.at("params").at("native").at("unknown").string()=="A");
  auto moved=std::move(value);CHECK(moved.at("params").at("native").kind==Json::Kind::object);
  CHECK(source.bytes(wire).front()=='{');
  bool rejected=false;try{(void)source.bytes("{}");}catch(const std::invalid_argument&){rejected=true;}CHECK(rejected);
  for(const auto bad:{R"({"params":{"native":{},"native":{}}})",R"({"params":{"native":{}}} trailing)"}){
   rejected=false;try{(void)parse_json_member(bad,memory,path,source);}catch(const std::invalid_argument&){rejected=true;}
   CHECK(rejected&&source.size==0);
  }
  (void)parse_json_member(R"({"params":[{"native":{}}]})",memory,path,source);CHECK(source.size==0);
 }
 std::string controls;for(char c=0;c<32;++c)controls+=c;
 {
  constexpr std::string_view path[]{"inputOriginal"};
  swegca::vrs::MemoryBudget small(4096);
  std::string large=R"({"payload":")"+std::string(1<<20,'x')+R"(","rows":[)";
  for(unsigned i=0;i<20000;++i){if(i)large+=',';large+=R"({"a":1,"b":"\u0041"})";}
  large+=R"(],"inputOriginal":{"id":"한글","extent":[1,2]},"tail":null})";
  bool exhausted=false;try{(void)parse_json(large,small);}catch(const std::bad_alloc&){exhausted=true;}
  CHECK(exhausted&&small.used()==0);
  {
   const auto selected=parse_json_selected(large,small,path);
   CHECK(selected.at("id").string()=="한글"&&selected.at("extent").values.size()==2);
   CHECK(small.peak_reserved()<=small.limit());
  }
  CHECK(small.used()==0);
  CHECK(parse_json_selected(R"({"inputOriginal":null})",small,path).kind==Json::Kind::null);
  CHECK(parse_json_selected(R"([1,true])",small,{}).values.size()==2);
  constexpr std::string_view nested[]{"outer","inputOriginal"};
  CHECK(parse_json_selected(R"({"outer":{"inputOriginal":"ok"},"inputOriginal":"wrong"})",small,nested).string()=="ok");
  for(const auto bad:{R"({"inputOriginal":{},"skip":{"a":1,"\u0061":2}})",
      R"({"inputOriginal":{},"skip":"\ud800"})",R"({"inputOriginal":{},"skip":[1,]})",
      R"({"inputOriginal":{},"skip":01})",R"({"inputOriginal":{}} trailing)",
      R"({"other":0})",R"({"outer":[{"inputOriginal":0}]})"}){
   bool rejected=false;try{(void)parse_json_selected(bad,small,path);}catch(const std::invalid_argument&){rejected=true;}
   CHECK(rejected&&small.used()==0);
  }
  bool rejected=false;
  try{(void)parse_json_selected(R"({"inputOriginal":{},"skip":[[[]]]})",small,path,2);}catch(const std::length_error&){rejected=true;}
  CHECK(rejected&&small.used()==0);
 }
 for(const auto& text:{std::string{},controls,std::string("한글🙂 quote\" slash\\ tail"),std::string(1<<20,'a')}){
  const auto expected=quote_json(text,memory);
  CHECK(parse_json(expected,memory).string()==text);
  {
   // This round-trip intentionally holds source, encoded and reparsed values
   // simultaneously; keep it separate from the output budget checks below.
   swegca::vrs::MemoryBudget roundtrip(8<<20);
   auto value=parse_json(expected,roundtrip);std::pmr::string message("{\"native\":",&roundtrip);
   append_json(message,value);message+='}';
   CHECK(parse_json(message,roundtrip).at("native").string()==text);
  }
  const auto used=memory.used();std::ostringstream out;
  write_json_string(out,text);CHECK(std::string_view(out.str())==std::string_view(expected));CHECK(memory.used()==used);
 }
 CHECK(memory.used()==0);
 // A final escaped quote must not double a large decoded envelope.
 {
  const std::string decoded=std::string("{\"text\":\"")+std::string(1<<20,'x')+"\"}";
  auto wire=quote_json(decoded,memory);
  swegca::vrs::MemoryBudget exact(decoded.size()+1);
  {auto value=parse_json(wire,exact);CHECK(value.string()==decoded);
   CHECK(exact.peak_reserved()==decoded.size()+1);}
  CHECK(exact.used()==0);
 }
 {
  const std::string wire=std::string("\"")+std::string(4096,'x')+
      R"(\u0000\u007f\u0080\u07ff\u0800\uffff\ud83d\ude42\"\\\/\b\f\n\r\t")";
  const std::string expected=std::string(4096,'x')+std::string(1,'\0')+
      "\x7f\xc2\x80\xdf\xbf\xe0\xa0\x80\xef\xbf\xbf\xf0\x9f\x99\x82"+
      "\"\\/\b\f\n\r\t";
  swegca::vrs::MemoryBudget exact(expected.size()+1);
  {auto value=parse_json(wire,exact);CHECK(value.string()==expected);}
  CHECK(exact.used()==0);
 }
 for(const auto tail:{R"(\uD800")",R"(\uDC00")",R"(\uD800\u0041")",
     R"(\uZZZZ")",R"(\q")","\\",""}){
  const auto wire=std::string("\"")+std::string(4096,'x')+tail;
  bool rejected=false;try{(void)parse_json(wire,memory);}catch(const std::invalid_argument&){rejected=true;}
  CHECK(rejected&&memory.used()==0);
 }
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
 for(const char byte:{'x','\n'}){
  Json value(&memory);value.kind=Json::Kind::string;value.scalar.assign(65536,byte);
  const auto expected=encode_json(value,memory);
  // Room for one encoded value and a small envelope, but not a doubled
  // output buffer or old/new output copies during growth.
  swegca::vrs::MemoryBudget output(expected.size()+64);
  std::pmr::string message("{\"native\":",&output);
  append_json(message,value,1);message+='}';
  CHECK(parse_json(message,memory).at("native").string()==value.scalar);
  CHECK(output.peak_reserved()<=expected.size()+64);
 }
 {
  auto value=parse_json("null",memory);std::pmr::string destination("unchanged",&memory);
  bool overflow=false;try{append_json(destination,value,std::numeric_limits<std::size_t>::max());}catch(const std::length_error&){overflow=true;}
  CHECK(overflow&&destination=="unchanged");
 }
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
 {
  swegca::vrs::MemoryBudget scratch(4096);
  constexpr std::array<std::string_view,2> path{"params","input"};
  const std::string raw="{\"params\":{\"inpu\\u0074\":[\""+std::string(1<<20,'x')+"\"]},\"tail\":true}";
  const auto source=locate_json_member(raw,scratch,path);
  CHECK(source.bytes(raw).size()==(1<<20)+4&&source.bytes(raw).front()=='[');
  CHECK(scratch.used()==0);
  CHECK(locate_json_member(" null ",scratch,{}).bytes(" null ")=="null");
  for(const auto bad:{R"({"params":{}})",R"({"params":{"input":[],"inpu\u0074":[]}})",
       R"({"params":{"input":[]},"tail":[1,]})",R"({"params":{"input":[]}} false)",
       R"({"params":{"input":["\uD800"]}})"}){
   bool rejected=false;try{(void)locate_json_member(bad,scratch,path);}catch(const std::invalid_argument&){rejected=true;}
   CHECK(rejected&&scratch.used()==0);
  }
 }
 std::printf("JSON stream tests: %u checks passed\n",checks);
}
