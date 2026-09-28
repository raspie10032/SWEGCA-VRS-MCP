#include "swegca_architecture/connection_strength_kernel.hpp"
// Read-only consumer of the sealed VRS graph, separate from VRS maintenance.
#include "vrs/experience_block.hpp"
#include "transport/json.hpp"
#include "swegca_architecture/recall_route_kernel.hpp"
#include "swegca_architecture/sha256.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>
#include <map>
#include <set>
#include <cmath>
#include <chrono>
#include <sys/resource.h>
using namespace swegca::architecture;
using namespace swegca::architecture::kernel;
using namespace swegca::vrs;
using namespace swegca::transport;
DigestBytes digest(std::string_view s){
 if(s.size()!=64)throw std::runtime_error("digest length");
 DigestBytes d{};
 auto nib=[](char c)->unsigned{if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;throw std::runtime_error("digest digit");};
 for(unsigned i=0;i<32;++i)d[i]=std::byte(nib(s[2*i])*16+nib(s[2*i+1]));
 return d;
}
std::string hex(const DigestBytes& d){std::string s;for(auto b:d){auto n=std::to_integer<unsigned>(b);s+="0123456789abcdef"[n>>4];s+="0123456789abcdef"[n&15];}return s;}
unsigned number(const Json& j){return std::stoul(std::string(j.scalar));}
RecordAddress address(const Json& j){return {digest(j.at("block").string()),std::stoull(std::string(j.at("offset").scalar)),std::stoull(std::string(j.at("bytes").scalar)),digest(j.at("digest").string())};}
std::string load(const std::filesystem::path& p){std::ifstream f(p);if(!f)throw std::runtime_error("missing "+p.string());return {std::istreambuf_iterator<char>(f),{}};}
struct Reader {
 MemoryBudget memory{128ULL<<20};std::map<DigestBytes,ExperienceBlock> blocks;
 explicit Reader(const std::filesystem::path& root){for(auto& e:std::filesystem::directory_iterator(root))if(e.path().extension()==".block"){auto b=ExperienceBlock::open_reader(e.path());auto id=b.identity();blocks.emplace(id,std::move(b));}}
 Json read(const RecordAddress& a){auto stored=blocks.at(a.block).read(a,2ULL<<20,memory);auto v=stored.view().content;return parse_json({reinterpret_cast<const char*>(v.data()),v.size()},memory);}
};
struct Candidate {ReplayCandidate replay;unsigned via=0;};
int main(int argc,char** argv)try{
 if(argc!=5&&argc!=6)throw std::runtime_error("usage: image-associations DATASET GRAPH CUE_JSON OUTPUT [--disconnect]");
 const bool disconnect=argc==6;if(disconnect&&std::string(argv[5])!="--disconnect")throw std::runtime_error("unknown option");
 rlimit cap{4000000000ULL,4000000000ULL};if(setrlimit(RLIMIT_AS,&cap))throw std::runtime_error("memory cap");
 const auto start=std::chrono::steady_clock::now();MemoryBudget scratch{128ULL<<20};
 const std::filesystem::path dataset(argv[1]),graph(argv[2]),cuepath(argv[3]);
 auto graph_summary=parse_json(load(graph/"summary.json"),scratch);
 if(graph_summary.at("schema").string()!="experience_pairs_common_member_v1")throw std::runtime_error("requires experience-pair graph");
 auto cue=parse_json(load(cuepath),scratch);auto query_sha=std::string(cue.at("image_sha256").string());
 Sha256 hash;hash.update(load(cuepath.parent_path()/"input.bin"));if(hex(hash.finish())!=query_sha)throw std::runtime_error("query raw image mismatch");
 if(cue.find("tags"))throw std::runtime_error("query tag leakage prohibited");
 const auto& q=cue.at("dino").values;if(q.size()!=384)throw std::runtime_error("DINO dimensions");
 std::vector<double> query;double qnorm=0;for(auto& n:q){double x=std::stod(std::string(n.scalar));if(!std::isfinite(x))throw std::runtime_error("nonfinite query");query.push_back(x);qnorm+=x*x;}if(qnorm==0)throw std::runtime_error("zero cue");
 std::string line;Reader reader(graph);std::map<unsigned,RecordAddress> sources,tags;std::vector<RecordAddress> edges;
 std::ifstream records(graph/"graph-records.jsonl");if(!records)throw std::runtime_error("graph index missing");
 while(std::getline(records,line)){auto r=parse_json(line,scratch);if(auto p=r.find("source"))sources.emplace(number(*p),address(r.at("record")));else if(auto p=r.find("tag"))tags.emplace(number(*p),address(r.at("record")));else if(r.find("links"))edges.push_back(address(r.at("record")));}
 // Scan the canonical sealed experiences, not repeated input deliveries.
 std::string seed_sha;unsigned index=0,seed=0;double best=-2;
 for(const auto& [source_id,location]:sources){
  auto incidence=reader.read(location);auto r=reader.read(address(incidence.at("feature_record")));
  auto sha=std::string(r.at("sha256").string());if(sha==query_sha)throw std::runtime_error("query is already in experience");
  if(r.at("models").at("dino_sha256").string()!=cue.at("dino_sha256").string())throw std::runtime_error("DINO model mismatch");
  auto& v=r.at("dino").values;if(v.size()!=query.size())throw std::runtime_error("stored dimension mismatch");
  double dot=0,norm=0;for(unsigned k=0;k<v.size();++k){double x=std::stod(std::string(v[k].scalar));if(!std::isfinite(x))throw std::runtime_error("stored nonfinite cue");dot+=query[k]*x;norm+=x*x;}
  if(norm==0)throw std::runtime_error("zero stored cue");
  double similarity=dot/std::sqrt(norm*qnorm);
  if(similarity>best||(similarity==best&&sha<seed_sha)){best=similarity;seed=source_id;seed_sha=sha;}++index;
 }
 if(!index||sources.size()!=number(graph_summary.at("images")))throw std::runtime_error("source count mismatch");
 auto source=reader.read(sources.at(seed));auto feature=reader.read(address(source.at("feature_record")));
 if(feature.at("sha256").string()!=seed_sha)throw std::runtime_error("selected source mismatch");
 // Authenticate the selected descriptor against its sealed experience.
 auto& sv=feature.at("dino").values;double dot=0,norm=0;for(unsigned k=0;k<query.size();++k){double x=std::stod(std::string(sv.at(k).scalar));dot+=query[k]*x;norm+=x*x;}
 if(dot/std::sqrt(norm*qnorm)!=best)throw std::runtime_error("selected cue differs from sealed experience");
 std::set<unsigned> seeds;for(auto& t:source.at("tags").values)seeds.insert(number(t));
 std::map<unsigned,Candidate> candidates;unsigned traversed=0;
 if(!disconnect)for(auto& a:edges){auto chunk=reader.read(a);for(auto& e:chunk.at("links").values){
  unsigned l=number(e.at("left")),r=number(e.at("right"));double strength=std::stod(std::string(e.at("strength").scalar));
  // Endpoints are experiences. Eligibility uses accumulated strength,
  // independently of the latest verdict. Weak records stay in storage.
  if(!connection_evidence_eligible(strength))continue;
  auto visit=[&](unsigned from,unsigned to){if(from!=seed)return;
   auto neighbor=reader.read(sources.at(to));
   for(auto& member:neighbor.at("tags").values){const auto tag=number(member);if(seeds.contains(tag))continue;
    (void)tags.at(tag);
    ReplayCandidate proposed{strength,0,a.digest,a};auto it=candidates.find(tag);
    auto preference=prefer_replay(it==candidates.end()?nullptr:&it->second.replay,proposed);
    if(preference==ReplayPreference::invalid)throw std::runtime_error("invalid persisted strength/address");
    if(preference==ReplayPreference::replace)candidates[tag]={proposed,to};
    ++traversed;
   }
  };visit(l,r);visit(r,l);
 }}
 std::vector<std::pair<unsigned,Candidate>> ranked(candidates.begin(),candidates.end());
 std::sort(ranked.begin(),ranked.end(),[](const auto& a,const auto& b){auto p=prefer_replay(&b.second.replay,a.second.replay);auto reverse=prefer_replay(&a.second.replay,b.second.replay);if(p==ReplayPreference::replace)return true;if(reverse==ReplayPreference::replace)return false;return a.first<b.first;});
 std::ofstream out(argv[4]);out.exceptions(std::ios::badbit|std::ios::failbit);
 out<<"{\"query_sha256\":";write_json_string(out,query_sha);out<<",\"seed_sha256\":";write_json_string(out,seed_sha);
 out<<",\"seed_source\":"<<seed<<",\"visual_cue_cosine\":"<<best<<",\"source_count\":"<<index<<",\"query_tags_used\":false,\"semantic_truth_claimed\":false,\"disconnected\":"<<(disconnect?"true":"false")<<",\"seed_tags\":[";
 bool comma=false;for(auto t:seeds){if(comma)out<<',';comma=true;auto node=reader.read(tags.at(t));write_json_string(out,node.at("tag").string());}
 out<<"],\"associated_tags\":[";comma=false;
 for(auto& [id,c]:ranked){if(comma)out<<',';comma=true;auto node=reader.read(tags.at(id));out<<"{\"tag\":";write_json_string(out,node.at("tag").string());out<<",\"via_experience\":"<<c.via;out<<",\"strength\":"<<c.replay.strength<<",\"record_digest\":\""<<hex(c.replay.original.digest)<<"\",\"record_block\":\""<<hex(c.replay.original.block)<<"\",\"record_offset\":"<<c.replay.original.offset<<'}';}
 out<<"],\"traversed_paths\":"<<traversed<<",\"seconds\":"<<std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count()<<"}\n";
 std::cout<<"seed="<<seed<<" cosine="<<best<<" seed_tags="<<seeds.size()<<" associated="<<ranked.size()<<" paths="<<traversed<<'\n';
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
