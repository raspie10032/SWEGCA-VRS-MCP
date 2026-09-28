#include "vrs/experience_block.hpp"
#include "swegca_architecture/sha256.hpp"
#include "transport/json.hpp"
#include <fstream>
#include <map>
#include <iostream>
using namespace swegca::vrs;using namespace swegca::architecture;using namespace swegca::transport;
std::string load(const std::filesystem::path& p){std::ifstream f(p);if(!f)throw std::runtime_error("missing checkpoint metadata");return {std::istreambuf_iterator<char>(f),{}};}
DigestBytes digest(std::string_view s){if(s.size()!=64)throw std::runtime_error("digest size");DigestBytes out{};auto nib=[](char c)->unsigned{if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;throw std::runtime_error("digest digit");};for(unsigned i=0;i<32;++i)out[i]=std::byte(nib(s[2*i])*16+nib(s[2*i+1]));return out;}
std::string hex(const DigestBytes& d){std::string s;for(auto b:d){auto n=std::to_integer<unsigned>(b);s+="0123456789abcdef"[n>>4];s+="0123456789abcdef"[n&15];}return s;}
std::uint64_t number(const Json& j){return std::stoull(std::string(j.scalar));}
struct Result {DigestBytes ti,tt;std::uint64_t images,tags,round,ti_count,tt_count;std::string graph;};
Result read(const std::filesystem::path& root){MemoryBudget memory(128ULL<<20);auto info=parse_json(load(root/"inputs.json"),memory);if(info.at("representation").string()!="accept_reject_abstain_u64")throw std::runtime_error("count schema");
 Result r{{},{},number(info.at("images")),number(info.at("tags")),number(info.at("rounds")),0,0,std::string(info.at("graph").string())};if(!r.images||r.tags<2||!r.round)throw std::runtime_error("dimensions");
 std::map<DigestBytes,ExperienceBlock> blocks;for(auto& entry:std::filesystem::directory_iterator(root))if(entry.path().extension()==".block"){auto block=ExperienceBlock::open_reader(entry.path());auto id=block.identity();blocks.emplace(id,std::move(block));}
 Sha256 ti,tt;std::ifstream f(root/"checkpoints.jsonl");if(!f)throw std::runtime_error("missing checkpoints");std::string line;
 while(std::getline(f,line)){auto row=parse_json(line,memory);if(number(row.at("round"))!=r.round)continue;auto kind=row.at("kind").string();if(kind!="tag_image"&&kind!="tag_tag")throw std::runtime_error("kind");auto& count=kind=="tag_image"?r.ti_count:r.tt_count;auto& hash=kind=="tag_image"?ti:tt;
  if(number(row.at("start"))!=count)throw std::runtime_error("noncontiguous checkpoint");const auto n=number(row.at("count"));if(n>(64ULL<<20)/24)throw std::runtime_error("chunk too large");const auto& a=row.at("record");RecordAddress address{digest(a.at("block").string()),number(a.at("offset")),number(a.at("bytes")),digest(a.at("digest").string())};auto record=blocks.at(address.block).read(address,64ULL<<20,memory);auto data=record.view().content;if(data.size()!=n*24)throw std::runtime_error("count bytes");hash.update(data);count+=n;
 }
 if(r.ti_count!=r.images*r.tags||r.tt_count!=r.tags*(r.tags-1)/2)throw std::runtime_error("incomplete checkpoint");r.ti=ti.finish();r.tt=tt.finish();return r;
}
int main(int argc,char**argv)try{if(argc!=3)throw std::runtime_error("usage: compare-count-checkpoints A B");auto a=read(argv[1]),b=read(argv[2]);bool same=a.images==b.images&&a.tags==b.tags&&a.round==b.round&&a.graph==b.graph&&a.ti==b.ti&&a.tt==b.tt;
 std::cout<<"{\"equal\":"<<(same?"true":"false")<<",\"round\":"<<a.round<<",\"tag_image_connections\":"<<a.ti_count<<",\"tag_tag_connections\":"<<a.tt_count<<",\"tag_image_sha256\":\""<<hex(a.ti)<<"\",\"tag_tag_sha256\":\""<<hex(a.tt)<<"\"}\n";return same?0:2;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
