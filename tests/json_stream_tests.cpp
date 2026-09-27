#include "transport/json.hpp"
#include "vrs/memory_budget.hpp"
#include <sstream>
#include <cstdio>
#include <cstdlib>
using namespace swegca::transport;
unsigned checks=0;
#define CHECK(x) do{++checks;if(!(x)){std::fprintf(stderr,"FAIL %d\n",__LINE__);std::abort();}}while(false)
int main(){
 swegca::vrs::MemoryBudget memory(4<<20);
 std::string controls;for(char c=0;c<32;++c)controls+=c;
 for(const auto& text:{std::string{},controls,std::string("한글🙂 quote\" slash\\ tail"),std::string(1<<20,'a')}){
  const auto expected=quote_json(text,memory);
  const auto used=memory.used();std::ostringstream out;
  write_json_string(out,text);CHECK(std::string_view(out.str())==std::string_view(expected));CHECK(memory.used()==used);
 }
 CHECK(memory.used()==0);
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
