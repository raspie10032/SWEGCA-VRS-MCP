#include "vrs/experience_block.hpp"
#include "swegca_architecture/association_kernel.hpp"
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
  OriginalExperienceView v{seq++,0,"tag-image","tag-image",media,content};auto need=ExperienceBlock::record_overhead+v.session.size()+v.source.size()+v.media_type.size()+content.size();
  if(!block||used+need>(64ULL<<20)){auto name="tag-image-"+std::to_string(index++)+".block";block.emplace(ExperienceBlock::create(root/name,key(root.string()+name),64ULL<<20,&storage));used=80;}
  auto a=block->append(v);used+=a.bytes;return a;
 }
};
std::uint64_t get64(std::span<const std::byte> bytes,std::size_t at){
 if(at>bytes.size()||bytes.size()-at<8)throw std::runtime_error("short matrix");
 std::uint64_t value=0;for(unsigned b=0;b<8;++b)value|=std::uint64_t(std::to_integer<unsigned>(bytes[at+b]))<<(8*b);return value;
}
void put64(std::vector<std::byte>& out,std::uint64_t n){for(unsigned b=0;b<8;++b)out.push_back(std::byte(n>>(8*b)));}
int main(int argc,char** argv)try{
 if(argc==4&&std::string(argv[1])=="inspect"){
  const std::filesystem::path root(argv[2]);const auto target=std::stoul(argv[3]);Reader reader(root);MemoryBudget memory{64ULL<<20};
  std::ifstream records(root/"tag-records.jsonl");std::string text;
  while(std::getline(records,text)){auto row=parse_json(text,memory);if(auto value=row.find("concept"))if(num(*value)==target){auto node=reader.read(addr(row.at("record")));std::cout<<encode_json(node,memory)<<'\n';return 0;}}
  throw std::runtime_error("concept not found");
 }
 if(argc!=5&&argc!=6)throw std::runtime_error("usage: tag-image-refinement prepare|apply DATASET PAIR_GRAPH OUTPUT [PREVIOUS]");
 rlimit cap{4000000000ULL,4000000000ULL};if(setrlimit(RLIMIT_AS,&cap))throw std::runtime_error("memory limit");
 const std::string mode(argv[1]);const std::filesystem::path data(argv[2]),graph(argv[3]),out(argv[4]);
 if(mode!="prepare"&&mode!="apply")throw std::runtime_error("mode");
 if(mode=="prepare"&&argc!=5)throw std::runtime_error("prepare takes no previous state");
 MemoryBudget scratch{128ULL<<20};Reader reader(graph);
 auto summary=parse_json(load(graph/"summary.json"),scratch);if(summary.at("schema").string()!="experience_pairs_common_member_v1")throw std::runtime_error("requires canonical pair graph");
 std::map<unsigned,RecordAddress> sources,tags;std::ifstream index(graph/"graph-records.jsonl");std::string line;
 while(std::getline(index,line)){auto r=parse_json(line,scratch);if(auto v=r.find("source"))sources.emplace(num(*v),addr(r.at("record")));else if(auto v=r.find("tag"))tags.emplace(num(*v),addr(r.at("record")));}
 const auto n=sources.size(),t=tags.size();if(n!=num(summary.at("images"))||t!=num(summary.at("tags")))throw std::runtime_error("incomplete graph index");
 std::vector<std::vector<std::uint32_t>> members(n);std::vector<BoundExperience> bindings(n);std::vector<std::string> image_ids(n);std::size_t max_members=1;
 for(auto& [i,a]:sources){if(i>=n)throw std::runtime_error("noncontiguous source");auto source=reader.read(a);auto fa=addr(source.at("feature_record"));auto feature=reader.read(fa);image_ids[i]=feature.at("sha256").string();
  auto actual=file_hash(data/"images"/(image_ids[i]+".bin"));auto scores=std::filesystem::path(feature.at("raw_scores").string());if(scores.is_absolute())throw std::runtime_error("score path");for(auto& part:scores)if(part=="..")throw std::runtime_error("score parent");
  bindings[i]=bind_experience(actual,digest(image_ids[i]),fa.digest,file_hash(data/scores));
  for(auto& value:source.at("tags").values){auto tag=num(value);if(tag>=t)throw std::runtime_error("unknown tag");members[i].push_back(tag);}max_members=std::max(max_members,members[i].size());
 }
 if(mode=="prepare"){
  if(std::filesystem::exists(out/"gpu-input.json"))throw std::runtime_error("prepare already exists");
  std::ofstream matrix(out/"members.i32",std::ios::binary);matrix.exceptions(std::ios::badbit|std::ios::failbit);
  for(auto& row:members)for(std::size_t j=0;j<max_members;++j){std::uint32_t v=j<row.size()?row[j]:UINT32_MAX;for(unsigned b=0;b<4;++b)matrix.put(static_cast<char>(v>>(b*8)));}
  matrix.close();
  std::ofstream meta(out/"gpu-input.json");meta.exceptions(std::ios::badbit|std::ios::failbit);meta<<"{\"images\":"<<n<<",\"tags\":"<<t<<",\"max_members\":"<<max_members<<",\"members_sha256\":\""<<hex(file_hash(out/"members.i32"))<<"\",\"graph_index_sha256\":\""<<hex(file_hash(graph/"graph-records.jsonl"))<<"\"}\n";
  std::cout<<"PREPARED images="<<n<<" tags="<<t<<" comparisons="<<n*t<<std::endl;return 0;
 }
 if(std::filesystem::exists(out/"concepts.jsonl"))throw std::runtime_error("apply already exists");
 auto meta=parse_json(load(out/"gpu-input.json"),scratch);
 if(num(meta.at("images"))!=n||num(meta.at("tags"))!=t||meta.at("members_sha256").string()!=hex(file_hash(out/"members.i32"))||meta.at("graph_index_sha256").string()!=hex(file_hash(graph/"graph-records.jsonl")))throw std::runtime_error("input changed");
 // The optional previous state is authenticated native state, not a CSV
 // strength override. Identical input observations must not be applied twice.
 std::unique_ptr<Reader> previous_reader;
 std::map<unsigned,RecordAddress> previous_records;
 if(argc==6){
  const std::filesystem::path previous(argv[5]);
  auto old_meta=parse_json(load(previous/"gpu-input.json"),scratch);
  auto old_summary=parse_json(load(previous/"summary.json"),scratch);
  if(old_meta.at("graph_index_sha256").string()!=meta.at("graph_index_sha256").string()||
     old_meta.at("members_sha256").string()!=meta.at("members_sha256").string()||
     num(old_summary.at("images"))!=n||num(old_summary.at("tags"))!=t||
     old_summary.at("schema").string()!="binary_tag_image_v2")
   throw std::runtime_error("previous state is not for these exact observations");
  previous_reader=std::make_unique<Reader>(previous);
  std::ifstream old_index(previous/"tag-records.jsonl");std::string text;
  while(std::getline(old_index,text)){auto entry=parse_json(text,scratch);if(auto tag=entry.find("tag"))
   if(!previous_records.emplace(num(*tag),addr(entry.at("record"))).second)throw std::runtime_error("duplicate previous matrix");}
  if(previous_records.size()!=t)throw std::runtime_error("incomplete previous state");
 }
 Writer writer{out};std::ofstream concepts(out/"concepts.jsonl"),receipts(out/"tag-records.jsonl");concepts.exceptions(std::ios::badbit|std::ios::failbit);receipts.exceptions(std::ios::badbit|std::ios::failbit);
 std::uint64_t totals[3]{},compared=0,concept_count=0,eligible_count=0;
 for(unsigned gpu=0;gpu<2;++gpu){
  const auto lo=t*gpu/2,hi=t*(gpu+1)/2;auto report=parse_json(load(out/("gpu"+std::to_string(gpu)+".json")),scratch);auto file=out/("gpu"+std::to_string(gpu)+".observations.u8");
  if(num(report.at("gpu"))!=gpu||num(report.at("tag_begin"))!=lo||num(report.at("tag_end"))!=hi||num(report.at("images"))!=n||report.at("members_sha256").string()!=meta.at("members_sha256").string()||report.at("output_sha256").string()!=hex(file_hash(file))||std::filesystem::file_size(file)!=(hi-lo)*n)throw std::runtime_error("GPU partition mismatch");
  std::ifstream observations(file,std::ios::binary);std::vector<unsigned char> row(n);
  for(std::size_t tag=lo;tag<hi;++tag){observations.read(reinterpret_cast<char*>(row.data()),n);if(!observations)throw std::runtime_error("GPU short read");
   auto node=reader.read(tags.at(tag));std::set<std::string> distinct_images;std::vector<unsigned> approved;std::vector<std::byte> payload;payload.reserve(24+n*16);put64(payload,tag);put64(payload,n);put64(payload,2);
   std::optional<StoredExperience> old_matrix;
   std::span<const std::byte> old_payload;
   if(previous_reader){
    const auto a=previous_records.at(tag);
    old_matrix.emplace(previous_reader->blocks.at(a.block).read(a,2ULL<<20,previous_reader->memory));
    old_payload=old_matrix->view().content;
    if(old_payload.size()!=24+n*16||get64(old_payload,0)!=tag||get64(old_payload,8)!=n||get64(old_payload,16)!=2)
     throw std::runtime_error("invalid previous matrix");
   }
   for(std::size_t image=0;image<n;++image){
    // Full CPU/core cross-check of GPU observations, not just a sample.
    bool recorded=std::find(members[image].begin(),members[image].end(),tag)!=members[image].end();
    if(row[image]>1||bool(row[image])!=recorded)throw std::runtime_error("GPU observation differs from original members");
    auto observed=to_outcome(observe_tag_image_match(bindings[image],members[image],tag));
    if(observed==EvidenceOutcome::insufficient)throw std::runtime_error("tag-image input is not ready for binary verification");
    const auto judgment=judge_association({observed==EvidenceOutcome::support?1ULL:0ULL,observed==EvidenceOutcome::refute?1ULL:0ULL});
    const double previous=previous_reader?std::bit_cast<double>(get64(old_payload,24+image*16+8)):1.0;
    if(!finite_count(previous))throw std::runtime_error("invalid previous strength");
    // Recompute the observation/verdict for checking, retain the accumulated
    // value when this identical evidence has already been applied.
    double current=previous;
    if(previous_reader){
     if(get64(old_payload,24+image*16)>2)throw std::runtime_error("invalid previous status");
    }else{
     const auto strength=revise_association_strength(previous,judgment);
     if(!strength.valid())throw std::runtime_error("invalid core result");
     current=strength.current();
    }
    auto status=static_cast<unsigned>(judgment.status());++totals[status];++compared;
    // Row order identifies canonical image source. Exact binary64 strength.
    put64(payload,status);put64(payload,std::bit_cast<std::uint64_t>(current));
    if(connection_evidence_eligible(current)){++eligible_count;approved.push_back(image);distinct_images.insert(image_ids[image]);}
   }
   auto location=writer.append(payload,"application/x-swegca-tag-image-v2");
   receipts<<"{\"tag\":"<<tag<<",\"record\":";address(receipts,location);receipts<<"}\n";
   const bool common=distinct_images.size()>1;concept_count+=common;
   std::ostringstream concept_record;concept_record<<"{\"tag\":"<<tag<<",\"name\":";write_json_string(concept_record,node.at("tag").string());concept_record<<",\"role\":\""<<(common?"common_concept":distinct_images.empty()?"unresolved":"single_observation")<<"\",\"distinct_images\":"<<distinct_images.size()<<",\"origin_graph\":";write_json_string(concept_record,graph.string());concept_record<<",\"origin_tag\":";address(concept_record,tags.at(tag));concept_record<<",\"verification\":";address(concept_record,location);concept_record<<",\"experiences\":[";for(std::size_t j=0;j<approved.size();++j){if(j)concept_record<<',';concept_record<<approved[j];}concept_record<<"]}";
   auto text=concept_record.str();auto ca=writer.append(std::as_bytes(std::span(text)),"application/json");concepts<<text<<'\n';receipts<<"{\"concept\":"<<tag<<",\"record\":";address(receipts,ca);receipts<<"}\n";
   if(tag%250==0)std::cout<<"core_verified_tags="<<tag+1<<'/'<<t<<" comparisons="<<compared<<std::endl;
  }
 }
 std::ofstream result(out/"summary.json");result.exceptions(std::ios::badbit|std::ios::failbit);result<<"{\"schema\":\"binary_tag_image_v2\",\"verification_mode\":\"binary\",\"images\":"<<n<<",\"tags\":"<<t<<",\"comparisons\":"<<compared<<",\"accept\":"<<totals[1]<<",\"reject\":"<<totals[2]<<",\"abstain\":"<<totals[0]<<",\"common_concepts\":"<<concept_count<<",\"eligible_connections\":"<<eligible_count<<",\"retained_ineligible_connections\":"<<(compared-eligible_count)<<",\"reapplied_observations\":0,\"duplicate_observations\":"<<(previous_reader?compared:0)<<",\"eligibility_minimum\":1,\"gpu_count\":2,\"all_gpu_observations_checked\":true,\"main_merged\":false}\n";
 std::cout<<"FINISHED comparisons="<<compared<<" accept="<<totals[1]<<" abstain="<<totals[0]<<" common_concepts="<<concept_count<<std::endl;
}catch(const std::exception& e){std::cerr<<"FAILED: "<<e.what()<<std::endl;return 1;}
