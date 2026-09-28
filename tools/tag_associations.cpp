#include "vrs/experience_block.hpp"
#include "swegca_architecture/evidence_rules.hpp"
#include "swegca_architecture/connection_strength_kernel.hpp"
#include "swegca_architecture/content_observation_kernel.hpp"
#include "swegca_architecture/association_kernel.hpp"
#include "swegca_architecture/sha256.hpp"
#include "transport/json.hpp"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <map>
#include <numeric>
#include <random>
#include <set>
#include <sstream>
#include <tuple>
#include <unordered_set>
#include <sys/resource.h>

using namespace swegca::architecture;
using namespace swegca::architecture::kernel;
using namespace swegca::vrs;
using namespace swegca::transport;
DigestBytes key(std::string_view s){Sha256 h;h.update(s);return h.finish();}
std::string hex(const DigestBytes& d){std::string s;for(auto b:d){unsigned n=std::to_integer<unsigned>(b);s+="0123456789abcdef"[n>>4];s+="0123456789abcdef"[n&15];}return s;}
std::span<const std::byte> bytes(std::string_view s){return std::as_bytes(std::span(s));}
void address(std::ostream& o,const ExperienceLocation& a){o<<"{\"block\":\""<<hex(a.block)<<"\",\"offset\":"<<a.offset<<",\"bytes\":"<<a.bytes<<",\"digest\":\""<<hex(a.digest)<<"\"}";}
struct Writer {
 std::filesystem::path root;std::string label;StorageBudget& storage;std::optional<ExperienceBlock> block{};unsigned index=0;std::uint64_t sequence=0,used=0;
 ExperienceLocation append(std::string_view text){
  OriginalExperienceView v{sequence++,0,"tag-links",label,"application/json",bytes(text)};
  const auto need=ExperienceBlock::record_overhead+v.session.size()+v.source.size()+v.media_type.size()+v.content.size();
  if(!block||need>(64ULL<<20)-used){
   block.reset();auto name=label+"-"+std::to_string(index++)+".block";
   block.emplace(ExperienceBlock::create(root/name,key(root.native()+"/"+name),64ULL<<20,&storage));
   used=ExperienceBlock::header_bytes;
  }
  auto position=block->append(v);used+=position.bytes;return position;
 }
};
struct Tag {std::string index,category,name,model;std::vector<std::uint32_t> sources;};
struct Input {std::string sha,model;std::vector<std::uint32_t> tags;BoundExperience binding;};
DigestBytes digest(std::string_view text){
 if(text.size()!=64)throw std::runtime_error("invalid source digest");
 DigestBytes d{};
 auto nibble=[](char c)->unsigned{if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;throw std::runtime_error("invalid digest digit");};
 for(unsigned i=0;i<32;++i)d[i]=std::byte((nibble(text[i*2])<<4)|nibble(text[i*2+1]));
 return d;
}
DigestBytes file_digest(const std::filesystem::path& path){
 std::ifstream in(path,std::ios::binary);if(!in)throw std::runtime_error("missing original: "+path.native());
 Sha256 h;std::array<char,65536> chunk;
 while(in.read(chunk.data(),chunk.size())||in.gcount())h.update(std::as_bytes(std::span(chunk.data(),static_cast<std::size_t>(in.gcount()))));
 if(!in.eof())throw std::runtime_error("original read failed");
 return h.finish();
}
void shuffle(std::vector<std::uint32_t>& values,std::uint64_t seed){
 std::mt19937_64 random(seed);
 for(std::size_t n=values.size();n>1;--n){auto floor=(std::uint64_t{0}-n)%n;std::uint64_t draw;do{draw=random();}while(draw<floor);std::swap(values[n-1],values[draw%n]);}
}
int main(int argc,char** argv)try{
 if(argc!=5){std::cerr<<"usage: tag-associations DATASET OUTPUT BINDING_RUN PREVIOUS_TAG_RUN\n";return 2;}
 struct rlimit cap{4000000000ULL,4000000000ULL};if(setrlimit(RLIMIT_AS,&cap))throw std::runtime_error("cannot set memory cap");
 const std::filesystem::path input(argv[1]),out(argv[2]);
 if(std::filesystem::exists(out/"sources.jsonl"))throw std::runtime_error("run already exists");
 MemoryBudget scratch(512ULL<<20);StorageBudget storage(500000000000ULL,0,625000000ULL);
 Writer originals{out,"originals",storage},graph{out,"graph",storage};
 std::ifstream in(input/"features.jsonl");if(!in)throw std::runtime_error("missing features");
 const std::filesystem::path prior(argv[3]),previous_graph(argv[4]);
 std::ifstream bindings(prior/"receipts.jsonl");if(!bindings)throw std::runtime_error("missing preserved image/feature/score bindings");
 std::ifstream previous_edges(previous_graph/"links.csv");if(!previous_edges)throw std::runtime_error("missing previous strengths");
 std::string previous_line;std::getline(previous_edges,previous_line);
 if(previous_line!="left,right,support,refute,observations,status,reason,previous_strength,strength,seed")throw std::runtime_error("unexpected prior schema");
 std::ofstream sources(out/"sources.jsonl"),nodes(out/"tags.jsonl"),edges(out/"links.csv"),heads(out/"graph-records.jsonl");
 for(auto* stream:{&sources,&nodes,&edges,&heads})stream->exceptions(std::ios::badbit|std::ios::failbit);
 std::map<std::tuple<std::string,std::string,std::string,std::string>,std::uint32_t> dictionary;
 std::vector<Tag> tags;std::vector<Input> inputs;std::unordered_set<std::uint64_t> candidates;
 std::unordered_set<std::string> seen;std::string line;
 const auto start=std::chrono::steady_clock::now();std::size_t verified_bindings=0;
 while(std::getline(in,line)){
  auto row=parse_json(line,scratch);Input sample;
  sample.sha=row.at("sha256").string();sample.model=row.at("models").at("tag_sha256").string();
  if(!seen.insert(sample.sha).second)throw std::runtime_error("duplicate source: no independent sample inflation allowed");
  const auto id=static_cast<std::uint32_t>(inputs.size());
  line+='\n';auto origin=originals.append(line);
  const auto actual=file_digest(input/"images"/(sample.sha+".bin"));
  const std::filesystem::path scores(row.at("raw_scores").string());
  if(scores.is_absolute())throw std::runtime_error("unexpected absolute score path");
  for(const auto& part:scores)if(part=="..")throw std::runtime_error("unexpected parent score path");
  // The user confirmed the existing image/tag/DINO pair. Authenticate its
  // actual raw image and preserve both processing payload digests. Legacy
  // blanket 'unverified' metadata is NOT a veto on this source-bound relation.
  const auto score_digest=file_digest(input/scores);
  sample.binding=bind_experience(actual,digest(sample.sha),origin.digest,score_digest);
  verified_bindings+=sample.binding.valid();
  sources<<"{\"source\":"<<id<<",\"image_sha256\":\""<<sample.sha<<"\",\"feature_record\":";address(sources,origin);
  sources<<",\"binding_verified\":"<<(sample.binding.valid()?"true":"false")<<",\"binding_basis\":\"user_confirmed_pair_and_authenticated_source\",\"raw_scores_digest\":\""<<hex(score_digest)<<"\",\"image_path\":";write_json_string(sources,(input/"images"/(sample.sha+".bin")).native());sources<<"}\n";
  for(const auto& tag:row.at("tags").values){
   const std::string index(tag.at("index").scalar),category(tag.at("category").scalar),name(tag.at("tag").string());
   auto [it,inserted]=dictionary.try_emplace(std::tuple{sample.model,index,category,name},static_cast<std::uint32_t>(tags.size()));
   if(inserted)tags.push_back({index,category,name,sample.model,{}});
   sample.tags.push_back(it->second);
  }
  std::sort(sample.tags.begin(),sample.tags.end());sample.tags.erase(std::unique(sample.tags.begin(),sample.tags.end()),sample.tags.end());
  // Preserve the existing native three-member experience as a graph member,
  // then connect its actual tag incidences into the ONE shared tag dictionary.
  // This is not a separate per-image refinement run or a new truth assertion.
  std::string binding_line;if(!std::getline(bindings,binding_line))throw std::runtime_error("missing source binding");
  const auto binding=parse_json(binding_line,scratch);
  if(binding.at("sha256").string()!=sample.sha)throw std::runtime_error("binding/source order mismatch");
  std::ostringstream incidence;incidence<<"{\"source\":"<<id<<",\"kind\":\"recorded_membership\",\"origin_store\":";
  write_json_string(incidence,(prior/"store").native());incidence<<",\"binding\":"<<encode_json(binding.at("binding"),scratch)<<",\"state\":"<<encode_json(binding.at("head"),scratch)<<",\"feature_record\":";
  address(incidence,origin);incidence<<",\"tags\":[";
  for(std::size_t t=0;t<sample.tags.size();++t){if(t)incidence<<',';incidence<<sample.tags[t];}incidence<<"],\"original_member_count\":3}";
  auto incidence_address=graph.append(incidence.str());heads<<"{\"source\":"<<id<<",\"record\":";address(heads,incidence_address);heads<<"}\n";
  // Repeated entries in the same preserved record are one membership, not
  // independently replicated observations. No score/category filter is added.
  for(auto t:sample.tags)tags[t].sources.push_back(id);
  for(std::size_t a=0;a<sample.tags.size();++a)for(std::size_t b=a+1;b<sample.tags.size();++b)
   candidates.insert((std::uint64_t(sample.tags[a])<<32)|sample.tags[b]);
  inputs.push_back(std::move(sample));
  if(inputs.size()%1000==0)std::cout<<"inputs="<<inputs.size()<<" unique_pairs="<<candidates.size()<<std::endl;
 }
 if(!in.eof())throw std::runtime_error("input stream failed");
 if(std::getline(bindings,line))throw std::runtime_error("unconsumed prior input binding");
 std::vector<std::uint32_t> parent(inputs.size()+tags.size());std::iota(parent.begin(),parent.end(),0);
 auto root=[&](std::uint32_t x){while(parent[x]!=x){parent[x]=parent[parent[x]];x=parent[x];}return x;};
 std::size_t isolated=0;std::uint64_t memberships=0;
 for(std::uint32_t s=0;s<inputs.size();++s){isolated+=inputs[s].tags.empty();for(auto t:inputs[s].tags){++memberships;auto a=root(s),b=root(static_cast<std::uint32_t>(inputs.size()+t));if(a!=b)parent[b]=a;}}
 std::set<std::uint32_t> components;for(std::uint32_t s=0;s<inputs.size();++s)components.insert(root(s));
 std::cout<<"GLOBAL_GRAPH input_bundles="<<inputs.size()<<" original_members="<<inputs.size()*3<<" tag_memberships="<<memberships<<" isolated_bundles="<<isolated<<" components="<<components.size()<<std::endl;
 for(std::size_t i=0;i<tags.size();++i){const auto& t=tags[i];
  std::ostringstream node;node<<"{\"id\":"<<i<<",\"model\":\""<<t.model<<"\",\"index\":"<<t.index<<",\"category\":"<<t.category<<",\"tag\":";
  write_json_string(node,t.name);node<<",\"sources\":[";for(std::size_t j=0;j<t.sources.size();++j){if(j)node<<',';node<<t.sources[j];}node<<"]}\n";
  nodes<<node.str();auto position=graph.append(node.str());heads<<"{\"tag\":"<<i<<",\"record\":";address(heads,position);heads<<"}\n";
 }
 sources.flush();nodes.flush();
 // Exact endpoint identity/order must match before reusing previous strengths.
 if(file_digest(out/"tags.jsonl")!=file_digest(previous_graph/"tags.jsonl"))throw std::runtime_error("prior tag endpoints changed");
 std::vector<std::uint64_t> pairs(candidates.begin(),candidates.end());candidates.clear();candidates.rehash(0);std::sort(pairs.begin(),pairs.end());
 const std::string definition="Verified input association: a source-bound image/tag/DINO experience jointly recording both tag endpoints witnesses a link. Missing one endpoint is not a refutation. Explicit contradictory evidence, if available, must target this same relation. Association judgment does not authorize World/semantic promotion.";
 auto definition_address=graph.append("{\"definition\":"+std::string(quote_json(definition,scratch))+"}");
 heads<<"{\"definition\":";address(heads,definition_address);heads<<"}\n";
 edges<<"left,right,support,refute,observations,status,reason,previous_strength,strength,seed\n";
 std::vector<std::uint32_t> union_sources,order;std::uint64_t statuses[3]{},reasons[10]{},total_observations=0,total_positive=0,total_negative=0,unrelated_absences=0;
 std::ostringstream chunk;std::size_t chunk_count=0;std::uint64_t completed=0;
 auto flush=[&]{if(!chunk_count)return;auto p=graph.append("{\"definition_record\":\""+hex(definition_address.digest)+"\",\"links\":["+chunk.str()+"]}");heads<<"{\"links\":"<<chunk_count<<",\"record\":";address(heads,p);heads<<"}\n";chunk.str("");chunk.clear();chunk_count=0;};
 for(auto pair:pairs){
  const auto a=static_cast<std::uint32_t>(pair>>32),b=static_cast<std::uint32_t>(pair);
  union_sources.clear();const auto& left=tags[a].sources;const auto& right=tags[b].sources;
  std::set_intersection(left.begin(),left.end(),right.begin(),right.end(),std::back_inserter(union_sources));
  unrelated_absences+=left.size()+right.size()-2*union_sources.size();
  order.resize(union_sources.size());std::iota(order.begin(),order.end(),0);const auto seed=1703ULL^pair;shuffle(order,seed);
  AssociationEvidence tally;
  std::uint64_t support=0,refute=0;
  for(auto ordinal:order){auto source=union_sources[ordinal];const auto& sample=inputs[source];
   const auto observation=observe_tag_association(sample.binding,sample.tags,a,b);
   if(observation==EvidenceOutcome::support){++support;++tally.support;}
   else if(observation==EvidenceOutcome::refute){++refute;++tally.refute;}
  }
  if(!std::getline(previous_edges,previous_line))throw std::runtime_error("missing prior edge");
  std::istringstream previous_row(previous_line);std::array<std::string,10> columns;
  for(auto& column:columns)if(!std::getline(previous_row,column,','))throw std::runtime_error("bad prior edge");
  if(std::stoul(columns[0])!=a||std::stoul(columns[1])!=b)throw std::runtime_error("prior edge identity mismatch");
  const auto previous=std::stod(columns[8]);
  const auto judgment=judge_association(tally);const auto strength=revise_association_strength(previous,judgment);
  if(!strength.valid())throw std::runtime_error("invalid core strength result");
  const auto status=static_cast<unsigned>(judgment.status()),reason=static_cast<unsigned>(judgment.reason());
  ++statuses[status];++reasons[reason];total_observations+=order.size();total_positive+=support;total_negative+=refute;
  edges<<a<<','<<b<<','<<support<<','<<refute<<','<<order.size()<<','<<status<<','<<reason<<','<<previous<<','<<strength.current()<<','<<seed<<'\n';
  if(chunk_count++)chunk<<',';
  chunk<<"{\"left\":"<<a<<",\"right\":"<<b<<",\"support\":"<<support<<",\"refute\":"<<refute<<",\"status\":"<<status<<",\"reason\":"<<reason<<",\"previous_strength\":"<<previous<<",\"strength\":"<<strength.current()<<",\"seed\":"<<seed<<"}";
  if(chunk_count==512)flush();
  if(++completed%25000==0){edges.flush();heads.flush();std::cout<<"pairs="<<completed<<'/'<<pairs.size()<<" accept="<<statuses[1]<<" reject="<<statuses[2]<<" abstain="<<statuses[0]<<std::endl;}
 }
 if(std::getline(previous_edges,previous_line))throw std::runtime_error("unconsumed prior edge");
 flush();edges.close();heads.close();sources.close();nodes.close();
 auto seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
 std::ofstream summary(out/"summary.json");summary.exceptions(std::ios::badbit|std::ios::failbit);
 summary<<"{\"images\":"<<inputs.size()<<",\"tags\":"<<tags.size()<<",\"pairs\":"<<pairs.size()<<",\"accept\":"<<statuses[1]<<",\"reject\":"<<statuses[2]<<",\"abstain\":"<<statuses[0]<<",\"pair_source_observations\":"<<total_observations<<",\"cooccurrences\":"<<total_positive<<",\"explicit_refutations\":"<<total_negative<<",\"unrelated_absences_not_refutations\":"<<unrelated_absences<<",\"verified_input_bindings\":"<<verified_bindings<<",\"seconds\":"<<seconds<<",\"native_storage_bytes\":"<<storage.used()<<",\"input_bundles_in_graph\":"<<inputs.size()<<",\"original_members_referenced\":"<<inputs.size()*3<<",\"recorded_membership_edges\":"<<memberships<<",\"isolated_input_bundles\":"<<isolated<<",\"membership_components\":"<<components.size()<<",\"general_four_axis_policy_unchanged\":true,\"judgment_scope\":\"verified_recorded_association\",\"main_merged\":false,\"extra_tag_threshold\":false,\"growth_claimed\":false}\n";
 summary.close();std::cout<<"FINISHED images="<<inputs.size()<<" tags="<<tags.size()<<" pairs="<<pairs.size()<<" accept="<<statuses[1]<<" reject="<<statuses[2]<<" abstain="<<statuses[0]<<" seconds="<<seconds<<std::endl;
}catch(const std::exception& e){std::cerr<<"FAILED: "<<e.what()<<std::endl;return 1;}
