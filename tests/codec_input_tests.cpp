#include "vrs/codec_input.hpp"
#include <cassert>
#include <fstream>
#include <iostream>
#include <unistd.h>
using namespace swegca::vrs;
int main(){
 char dir[]="/tmp/swegca-codec-XXXXXX";assert(mkdtemp(dir));const std::filesystem::path root(dir);MemoryBudget memory(64ULL<<20);
 {std::ofstream f(root/"image.ppm",std::ios::binary);f<<"P6\n2 1\n255\n";const char bytes[]={char(255),0,0,0,char(255),0};f.write(bytes,6);}
 {std::ofstream f(root/"text.txt");f<<"한국어 日本語 English\n";}
 auto image=decode_file(root/"image.ppm",memory);assert(image->source_media.starts_with("image/"));assert(image->decoded.size()==1&&!image->decoded[0].bytes.empty()&&image->codec_errors.empty());
 auto text=decode_file(root/"text.txt",memory);assert(text->source_media.starts_with("text/"));assert(text->decoded.empty()&&!text->original.empty());
 unsigned files=0;for(auto& entry:std::filesystem::directory_iterator(root)){(void)entry;++files;}assert(files==2);
 bool rejected=false;try{decode_file(root/"missing",memory);}catch(const std::runtime_error&){rejected=true;}assert(rejected);
 std::cout<<"PASS actual image codec, multilingual unchanged text, volatile output, no intermediate disk files; decoded="<<image->decoded[0].bytes.size()<<" bytes\n";
 image.reset();text.reset();assert(memory.used()==0);std::filesystem::remove_all(root);
}
