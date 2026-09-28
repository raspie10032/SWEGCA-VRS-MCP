#include "vrs/experience_block.hpp"
#include "swegca_architecture/association_kernel.hpp"
#include "swegca_architecture/cooccurrence_kernel.hpp"
#include "swegca_architecture/sha256.hpp"
#include "transport/json.hpp"
#include <fstream>
#include <bit>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <iomanip>
#include <sys/resource.h>
using namespace swegca::architecture;
using namespace swegca::architecture::kernel;
using namespace swegca::vrs;
using namespace swegca::transport;
DigestBytes key(std::string_view s){Sha256 h;h.update(s);return h.finish();}
std::string hex(const DigestBytes& d){std::string s;for(auto b:d){auto n=std::to_integer<unsigned>(b);s+="0123456789abcdef"[n>>4];s+="0123456789abcdef"[n&15];}return s;}
DigestBytes digest(std::string_view s){
 if(s.size()!=64)throw std::runtime_error("digest length");
 DigestBytes d{};auto nib=[](char c)->unsigned{if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;throw std::runtime_error("digest digit");};
 for(unsigned i=0;i<32;++i)d[i]=std::byte(nib(s[2*i])*16+nib(s[2*i+1]));
 return d;
}
DigestBytes file_hash(const std::filesystem::path& p){std::ifstream in(p,std::ios::binary);if(!in)throw std::runtime_error("missing "+p.string());Sha256 h;std::array<char,65536> b;while(in.read(b.data(),b.size())||in.gcount())h.update(std::as_bytes(std::span(b.data(),static_cast<std::size_t>(in.gcount()))));if(!in.eof())throw std::runtime_error("read error");return h.finish();}
std::string load(const std::filesystem::path& p){std::ifstream f(p);if(!f)throw std::runtime_error("missing "+p.string());return {std::istreambuf_iterator<char>(f),{}};}
unsigned num(const Json& j){return std::stoul(std::string(j.scalar));}
RecordAddress addr(const Json& j){return {digest(j.at("block").string()),std::stoull(std::string(j.at("offset").scalar)),std::stoull(std::string(j.at("bytes").scalar)),digest(j.at("digest").string())};}
void address(std::ostream& o,const RecordAddress& a){o<<"{\"block\":\""<<hex(a.block)<<"\",\"offset\":"<<a.offset<<",\"bytes\":"<<a.bytes<<",\"digest\":\""<<hex(a.digest)<<"\"}";}
struct Reader{
 MemoryBudget memory{128ULL<<20};std::map<DigestBytes,ExperienceBlock> blocks;
 explicit Reader(const std::filesystem::path& p){for(auto& e:std::filesystem::directory_iterator(p))if(e.path().extension()==".block"){auto b=ExperienceBlock::open_reader(e.path());auto id=b.identity();blocks.emplace(id,std::move(b));}}
 Json read(const RecordAddress& a){auto x=blocks.at(a.block).read(a,2ULL<<20,memory);auto bytes=x.view().content;return parse_json({reinterpret_cast<const char*>(bytes.data()),bytes.size()},memory);}
};
struct Writer{
 std::filesystem::path root;StorageBudget storage{500000000000ULL,0,625000000ULL};std::optional<ExperienceBlock> block{};unsigned index=0;std::uint64_t used=0,seq=0;
 RecordAddress append(std::span<const std::byte> content,std::string_view media){
  OriginalExperienceView v{seq++,0,"tag-tag","tag-tag",media,content};auto need=ExperienceBlock::record_overhead+v.session.size()+v.source.size()+v.media_type.size()+content.size();
  if(!block||used+need>(64ULL<<20)){auto name="tag-tag-"+std::to_string(index++)+".block";block.emplace(ExperienceBlock::create(root/name,key(root.string()+name),64ULL<<20,&storage));used=80;}
  auto a=block->append(v);used+=a.bytes;return a;
 }
};
std::uint64_t get64(std::span<const std::byte> bytes,std::size_t at){
 if(at>bytes.size()||bytes.size()-at<8)throw std::runtime_error("short matrix");
 std::uint64_t value=0;for(unsigned b=0;b<8;++b)value|=std::uint64_t(std::to_integer<unsigned>(bytes[at+b]))<<(8*b);return value;
}
void put64(std::vector<std::byte>& out,std::uint64_t n){for(unsigned b=0;b<8;++b)out.push_back(std::byte(n>>(8*b)));}
int main(int argc,char** argv)try{
 if(argc!=3)throw std::runtime_error("usage: tag-tag-refinement TAG_IMAGE_STATE OUTPUT");
 rlimit cap{4000000000ULL,4000000000ULL};if(setrlimit(RLIMIT_AS,&cap))throw std::runtime_error("memory limit");
 const std::filesystem::path input(argv[1]),out(argv[2]);
 if(std::filesystem::exists(out/"pairs.jsonl"))throw std::runtime_error("output already exists");
 MemoryBudget scratch{128ULL<<20};Reader reader(input);
 auto summary=parse_json(load(input/"summary.json"),scratch);
 if(summary.at("schema").string()!="binary_tag_image_v2")throw std::runtime_error("wrong input schema");
 const auto n=num(summary.at("images")),t=num(summary.at("tags"));
 if(!n||t<2)throw std::runtime_error("empty inputs");
 const std::size_t words=(n+63)/64;
 std::vector<std::vector<std::uint64_t>> known(t,std::vector<std::uint64_t>(words,UINT64_MAX)),present(t,std::vector<std::uint64_t>(words));
 std::map<unsigned,RecordAddress> matrices;
 std::ifstream index(input/"tag-records.jsonl");std::string line;
 while(std::getline(index,line)){auto entry=parse_json(line,scratch);if(auto tag=entry.find("tag"))if(!matrices.emplace(num(*tag),addr(entry.at("record"))).second)throw std::runtime_error("duplicate tag");}
 if(matrices.size()!=t)throw std::runtime_error("incomplete tags");
 // Original tag-image verdicts are observations here. Weak accumulated
 // strength does not remove an observation from collision verification.
 for(const auto& [tag,a]:matrices){
  if(tag>=t)throw std::runtime_error("tag range");
  auto stored=reader.blocks.at(a.block).read(a,2ULL<<20,reader.memory);auto bytes=stored.view().content;
  if(bytes.size()!=24+std::size_t(n)*16||get64(bytes,0)!=tag||get64(bytes,8)!=n||get64(bytes,16)!=2)throw std::runtime_error("matrix format");
  for(unsigned image=0;image<n;++image){auto status=get64(bytes,24+std::size_t(image)*16);const auto mask=std::uint64_t(1)<<(image%64);
   if(status==1)present[tag][image/64]|=mask;
   else if(status==0)known[tag][image/64]&=~mask;
   else if(status!=2)throw std::runtime_error("invalid observation status");
  }
 }
 Writer writer{out};std::ofstream receipts(out/"pairs.jsonl");receipts.exceptions(std::ios::badbit|std::ios::failbit);
 std::vector<std::byte> payload;payload.reserve(4096*40);std::uint64_t totals[3]{},pairs=0,eligible=0;
 auto flush=[&]{if(payload.empty())return;auto a=writer.append(payload,"application/x-swegca-tag-tag-v1");receipts<<"{\"pairs\":"<<payload.size()/40<<",\"record\":";address(receipts,a);receipts<<"}\n";payload.clear();};
 for(unsigned left=0;left<t;++left){
  for(unsigned right=left+1;right<t;++right){
   const auto observation=observe_cooccurrence({known[left],present[left]},{known[right],present[right]});
   const auto outcome=to_outcome(observation.predicate);
   const auto judgment=judge_association({outcome==EvidenceOutcome::support?1ULL:0ULL,outcome==EvidenceOutcome::refute?1ULL:0ULL});
   const auto strength=revise_association_strength(1,judgment);if(!strength.valid())throw std::runtime_error("invalid strength");
   const auto status=static_cast<unsigned>(judgment.status());++totals[status];++pairs;eligible+=strength.evidence_eligible();
   put64(payload,left);put64(payload,right);put64(payload,observation.witnesses);put64(payload,status);put64(payload,std::bit_cast<std::uint64_t>(strength.current()));
   if(payload.size()==4096*40)flush();
  }
  if(left%250==0)std::cout<<"tags="<<left+1<<'/'<<t<<" pairs="<<pairs<<std::endl;
 }
 flush();receipts.close();
 std::ofstream provenance(out/"evidence.json");provenance.exceptions(std::ios::badbit|std::ios::failbit);
 provenance<<"{\"input\":";write_json_string(provenance,input.string());provenance<<",\"input_index_sha256\":\""<<hex(file_hash(input/"tag-records.jsonl"))<<"\",\"definition\":\"There exists a canonical image experience containing both tags\",\"matrix_records\":[";
 bool first=true;for(auto& [tag,a]:matrices){if(!first)provenance<<',';first=false;provenance<<"{\"tag\":"<<tag<<",\"record\":";address(provenance,a);provenance<<'}';}provenance<<"]}\n";
 std::ofstream result(out/"summary.json");result.exceptions(std::ios::badbit|std::ios::failbit);
 result<<"{\"schema\":\"tag_tag_cooccurrence_v1\",\"images\":"<<n<<",\"tags\":"<<t<<",\"pairs\":"<<pairs<<",\"accept\":"<<totals[1]<<",\"reject\":"<<totals[2]<<",\"abstain\":"<<totals[0]<<",\"eligible\":"<<eligible<<",\"retained_ineligible\":"<<pairs-eligible<<",\"initial_strength\":1,\"main_merged\":false}\n";
 std::cout<<"FINISHED pairs="<<pairs<<" accept="<<totals[1]<<" reject="<<totals[2]<<" abstain="<<totals[0]<<std::endl;
}catch(const std::exception& e){std::cerr<<"FAILED: "<<e.what()<<std::endl;return 1;}
