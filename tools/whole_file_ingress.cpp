#include "vrs/block_ingress.hpp"
#include "transport/json.hpp"
#include <charconv>
#include <iostream>
#include <mutex>
#include <sstream>
#include <sys/stat.h>
using namespace swegca::vrs;using namespace swegca::architecture;using namespace swegca::transport;
std::uint64_t integer(std::string_view value){std::uint64_t n;auto [end,ec]=std::from_chars(value.data(),value.data()+value.size(),n);if(ec!=std::errc{}||end!=value.data()+value.size())throw std::invalid_argument("invalid unsigned integer");return n;}
DigestBytes digest(std::string_view text){if(text.size()!=64)throw std::invalid_argument("peer digest length");DigestBytes out{};for(std::size_t i=0;i<32;++i){unsigned n;auto [end,ec]=std::from_chars(text.data()+2*i,text.data()+2*i+2,n,16);if(ec!=std::errc{}||end!=text.data()+2*i+2)throw std::invalid_argument("peer digest encoding");out[i]=std::byte(n);}return out;}
int main(int argc,char** argv)try{
 if(argc<2)throw std::invalid_argument("usage: whole-file-ingress ROOT [--gpus N] [--block-originals N]");
 umask(0077);BlockIngress::Config config;
#ifndef SWEGCA_GPU_CORE
 config.gpu_devices=0;
#endif
 for(int i=2;i<argc;i+=2){if(i+1==argc)throw std::invalid_argument("missing option value");const std::string_view option(argv[i]);const auto n=integer(argv[i+1]);if(option=="--gpus"){if(n>2)throw std::invalid_argument("at most two GPUs");config.gpu_devices=n;}else if(option=="--mix-unknown")config.mix_unknown=n!=0;else if(option=="--block-originals")config.originals_per_block=n;else throw std::invalid_argument("unknown option");}
 std::mutex output;MemoryBudget parse_memory(64ULL<<20);
 BlockIngress ingress(argv[1],config,[&](const BlockIngress::Receipt& receipt){
  if(!receipt.error.empty()){std::lock_guard lock(output);std::cout<<"{\"ticket\":"<<receipt.ticket<<",\"error\":";write_json_string(std::cout,receipt.error);std::cout<<"}"<<std::endl;return;}
  std::ostringstream row;row<<"{\"ticket\":"<<receipt.ticket<<",\"block\":"<<receipt.block<<",\"duplicate\":"<<(receipt.duplicate?"true":"false");
  if(!receipt.duplicate){row<<",\"status\":"<<unsigned(receipt.status)<<",\"original\":{\"block\":\"";write_json_hex(row,receipt.original.block);row<<"\",\"offset\":"<<receipt.original.offset<<",\"bytes\":"<<receipt.original.bytes<<",\"digest\":\"";write_json_hex(row,receipt.original.digest);row<<"\"}";}
  row<<",\"experience_id\":\"";write_json_hex(row,receipt.identity);row<<"\"";
  row<<",\"relation_counts\":["<<receipt.relations.accept<<','<<receipt.relations.reject<<','<<receipt.relations.abstain<<"],\"backend\":";write_json_string(row,receipt.backend);row<<",\"decoded_views\":"<<receipt.decoded_views<<",\"codec_errors\":"<<receipt.codec_errors<<"}";
  std::lock_guard lock(output);std::cout<<row.str()<<std::endl;
 });
 std::string line;while(std::getline(std::cin,line)){
  auto json=parse_json(line,parse_memory);BlockFileRequest request;request.path=std::string(json.at("path").string());request.source=json.at("source").string();request.media=json.at("media").string();request.ticket=integer(json.at("ticket").scalar);
  // Optional already-observed relation evidence arrives WITH the input. The
  // codec is not a semantic classifier and absence is not negative evidence.
  if(const auto* relations=json.find("relations")){
   if(relations->kind!=Json::Kind::array)throw std::invalid_argument("relations must be an array");
   request.observation=std::string(encode_json(*relations,parse_memory));
   for(const auto& relation:relations->values)request.relations.push_back({digest(relation.at("peer").string()),{integer(relation.at("support").scalar),integer(relation.at("refute").scalar)}});
  }
  ingress.submit(std::move(request));
 }
 ingress.finish();const auto stats=ingress.stats();std::cerr<<"completed submitted="<<stats.submitted<<" decoded="<<stats.decoded<<" verified="<<stats.verified<<" applied="<<stats.applied<<" block_batches="<<stats.apply_batches<<'\n';
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
