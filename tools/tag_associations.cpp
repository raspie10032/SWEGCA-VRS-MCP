#include "vrs/experience_block.hpp"
#include "vrs/experience_pairs.hpp"
#include <iomanip>
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
#include <unordered_map>
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
struct Tag {std::string index,category,name,model;std::vector<std::uint32_t> sources,witnesses;};
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
int main(int argc,char** argv)try{
 if(argc!=6){std::cerr<<"usage: tag-associations DATASET OUTPUT BINDING_RUN PREVIOUS_PAIR_RUN_OR_MINUS SHUFFLE_SEED\n";return 2;}
 struct rlimit cap{4000000000ULL,4000000000ULL};if(setrlimit(RLIMIT_AS,&cap))throw std::runtime_error("cannot set memory cap");
 const std::filesystem::path input(argv[1]),out(argv[2]);
 if(std::filesystem::exists(out/"sources.jsonl"))throw std::runtime_error("run already exists");
 MemoryBudget scratch(512ULL<<20);StorageBudget storage(500000000000ULL,0,625000000ULL);
 Writer originals{out,"originals",storage},graph{out,"graph",storage};
 std::ifstream in(input/"features.jsonl");if(!in)throw std::runtime_error("missing features");
 const std::filesystem::path prior(argv[3]),previous_graph(argv[4]);
 std::ifstream bindings(prior/"receipts.jsonl");if(!bindings)throw std::runtime_error("missing preserved image/feature/score bindings");
 const bool fresh=previous_graph=="-";
 const auto shuffle_seed=std::stoull(argv[5]);
 if(!fresh){std::ifstream f(previous_graph/"summary.json");std::string text((std::istreambuf_iterator<char>(f)),{});
  auto prior_summary=parse_json(text,scratch);
  if(prior_summary.at("schema").string()!="experience_pairs_common_member_v1")throw std::runtime_error("previous graph is not an experience-pair graph");}
 std::ofstream aliases(out/"aliases.jsonl");aliases.exceptions(std::ios::badbit|std::ios::failbit);
 std::ofstream sources(out/"sources.jsonl"),nodes(out/"tags.jsonl"),edges(out/"links.csv"),heads(out/"graph-records.jsonl");
 for(auto* stream:{&sources,&nodes,&edges,&heads}){stream->exceptions(std::ios::badbit|std::ios::failbit);*stream<<std::setprecision(17);}
 std::map<std::tuple<std::string,std::string,std::string,std::string>,std::uint32_t> dictionary;
 std::vector<Tag> tags;std::vector<Input> inputs;std::vector<RecordAddress> original_addresses;
 std::unordered_map<std::string,std::vector<std::size_t>> identities;
 std::vector<std::string> original_feature_bytes;std::vector<std::string> experience_ids;
 std::uint64_t input_occurrences=0,duplicate_occurrences=0;std::string line;
 const auto start=std::chrono::steady_clock::now();std::size_t verified_bindings=0;
 while(std::getline(in,line)){
  auto row=parse_json(line,scratch);Input sample;
  sample.sha=row.at("sha256").string();sample.model=row.at("models").at("tag_sha256").string();
  line+='\n';
  const auto actual=file_digest(input/"images"/(sample.sha+".bin"));
  const std::filesystem::path scores(row.at("raw_scores").string());
  if(scores.is_absolute())throw std::runtime_error("unexpected absolute score path");
  for(const auto& part:scores)if(part=="..")throw std::runtime_error("unexpected parent score path");
  // The user confirmed the existing image/tag/DINO pair. Authenticate its
  // actual raw image and preserve both processing payload digests. Legacy
  // blanket 'unverified' metadata is NOT a veto on this source-bound relation.
  const auto score_digest=file_digest(input/scores);

  std::string binding_line;if(!std::getline(bindings,binding_line))throw std::runtime_error("missing source binding");
  const auto binding=parse_json(binding_line,scratch);
  if(binding.at("sha256").string()!=sample.sha)throw std::runtime_error("binding/source order mismatch");
  // Identity covers the complete image + feature-record + raw-score payloads.
  // Repeated deliveries retain provenance but never become another witness.
  const auto identity=hex(key(hex(actual)+hex(key(line))+hex(score_digest)));
  auto& bucket=identities[identity];std::optional<std::size_t> duplicate;
  for(auto candidate:bucket)if(original_feature_bytes[candidate]==line){duplicate=candidate;break;}
  const auto id=static_cast<std::uint32_t>(duplicate?*duplicate:inputs.size());
  std::ostringstream delivery;delivery<<"{\"input_occurrence\":"<<input_occurrences++<<",\"source\":"<<id<<",\"duplicate\":"<<(duplicate?"true":"false")<<",\"receipt\":"<<encode_json(binding,scratch)<<"}";
  auto delivery_address=graph.append(delivery.str());aliases<<delivery.str()<<'\n';
  heads<<"{\"delivery\":"<<input_occurrences-1<<",\"record\":";address(heads,delivery_address);heads<<"}\n";
  if(duplicate){++duplicate_occurrences;continue;}
  bucket.push_back(id);original_feature_bytes.push_back(line);experience_ids.push_back(identity);
  auto origin=originals.append(line);
  sample.binding=bind_experience(actual,digest(sample.sha),origin.digest,score_digest);
  verified_bindings+=sample.binding.valid();
  sources<<"{\"source\":"<<id<<",\"image_sha256\":\""<<sample.sha<<"\",\"feature_record\":";address(sources,origin);
  sources<<",\"experience_identity\":\""<<identity<<"\",\"binding_verified\":"<<(sample.binding.valid()?"true":"false")<<",\"binding_basis\":\"user_confirmed_pair_and_authenticated_source\",\"raw_scores_digest\":\""<<hex(score_digest)<<"\",\"image_path\":";write_json_string(sources,(input/"images"/(sample.sha+".bin")).native());sources<<"}\n";
  for(const auto& tag:row.at("tags").values){
   const std::string index(tag.at("index").scalar),category(tag.at("category").scalar),name(tag.at("tag").string());
   auto [it,inserted]=dictionary.try_emplace(std::tuple{sample.model,index,category,name},static_cast<std::uint32_t>(tags.size()));
   if(inserted)tags.push_back({index,category,name,sample.model,{},{}});
   sample.tags.push_back(it->second);
  }
  std::sort(sample.tags.begin(),sample.tags.end());sample.tags.erase(std::unique(sample.tags.begin(),sample.tags.end()),sample.tags.end());
  // Preserve the existing native three-member experience as a graph member,
  // then connect its actual tag incidences into the ONE shared tag dictionary.
  // This is not a separate per-image refinement run or a new truth assertion.
  std::ostringstream incidence;incidence<<"{\"source\":"<<id<<",\"kind\":\"recorded_membership\",\"origin_store\":";
  write_json_string(incidence,(prior/"store").native());incidence<<",\"binding\":"<<encode_json(binding.at("binding"),scratch)<<",\"state\":"<<encode_json(binding.at("head"),scratch)<<",\"feature_record\":";
  address(incidence,origin);incidence<<",\"tags\":[";
  for(std::size_t t=0;t<sample.tags.size();++t){if(t)incidence<<',';incidence<<sample.tags[t];}incidence<<"],\"original_member_count\":3}";
  auto incidence_address=graph.append(incidence.str());original_addresses.push_back(incidence_address);heads<<"{\"source\":"<<id<<",\"record\":";address(heads,incidence_address);heads<<"}\n";
  // Repeated entries in the same preserved record are one membership, not
  // independently replicated observations. No score/category filter is added.
  for(auto t:sample.tags)tags[t].sources.push_back(id);
  inputs.push_back(std::move(sample));
  if(inputs.size()%1000==0)std::cout<<"inputs="<<inputs.size()<<std::endl;
 }
 if(!in.eof())throw std::runtime_error("input stream failed");
 if(std::getline(bindings,line))throw std::runtime_error("unconsumed prior input binding");
 std::vector<std::uint32_t> parent(inputs.size()+tags.size());std::iota(parent.begin(),parent.end(),0);
 auto root=[&](std::uint32_t x){while(parent[x]!=x){parent[x]=parent[parent[x]];x=parent[x];}return x;};
 std::size_t isolated=0;std::uint64_t memberships=0;
 for(std::uint32_t s=0;s<inputs.size();++s){isolated+=inputs[s].tags.empty();for(auto t:inputs[s].tags){++memberships;auto a=root(s),b=root(static_cast<std::uint32_t>(inputs.size()+t));if(a!=b)parent[b]=a;}}
 std::set<std::uint32_t> components;for(std::uint32_t s=0;s<inputs.size();++s)components.insert(root(s));
 std::cout<<"GLOBAL_GRAPH input_bundles="<<inputs.size()<<" original_members="<<inputs.size()*3<<" tag_memberships="<<memberships<<" isolated_bundles="<<isolated<<" components="<<components.size()<<std::endl;
 for(std::size_t i=0;i<tags.size();++i){auto& t=tags[i];
  // Index observations across ALL incoming experiences, never a shared verdict.
  for(auto source:t.sources)if(observe_recorded_member(inputs[source].binding,inputs[source].tags,i)==EvidenceOutcome::support)t.witnesses.push_back(source);
  std::ostringstream node;node<<"{\"id\":"<<i<<",\"model\":\""<<t.model<<"\",\"index\":"<<t.index<<",\"category\":"<<t.category<<",\"tag\":";
  write_json_string(node,t.name);node<<",\"sources\":[";for(std::size_t j=0;j<t.sources.size();++j){if(j)node<<',';node<<t.sources[j];}node<<"],\"witnesses\":[";for(std::size_t j=0;j<t.witnesses.size();++j){if(j)node<<',';node<<t.witnesses[j];}node<<"]}\n";
  nodes<<node.str();auto position=graph.append(node.str());heads<<"{\"tag\":"<<i<<",\"record\":";address(heads,position);heads<<"}\n";
 }
 sources.flush();nodes.flush();
 const auto count=inputs.size();
 const auto pair_count=count?count*(count-1)/2:0;
 auto ordinal=[&](std::size_t a,std::size_t b){return a*(2*count-a-1)/2+b-a-1;};
 std::vector<double> prior_weights;
 if(!fresh){
  if(file_digest(out/"tags.jsonl")!=file_digest(previous_graph/"tags.jsonl"))throw std::runtime_error("prior member identity changed");
  std::ifstream src(previous_graph/"sources.jsonl");std::string old;
  for(auto& identity:experience_ids){if(!std::getline(src,old)||parse_json(old,scratch).at("experience_identity").string()!=identity)throw std::runtime_error("prior experience identity changed");}
  if(std::getline(src,old))throw std::runtime_error("extra previous experience");
  prior_weights.assign(pair_count,-1);
  std::ifstream prev(previous_graph/"links.csv");std::getline(prev,old);
  if(old!="left,right,support,refute,observations,status,reason,previous_strength,strength,seed")throw std::runtime_error("previous pair columns");
  std::size_t read=0;
  while(std::getline(prev,old)){
   std::istringstream row(old);std::array<std::string,10> columns;
   for(auto& column:columns)if(!std::getline(row,column,','))throw std::runtime_error("incomplete pair state");
   const auto a=std::stoull(columns[0]),b=std::stoull(columns[1]);
   if(a>=b||b>=count)throw std::runtime_error("pair identity invalid");
   auto& slot=prior_weights.at(ordinal(a,b));if(slot!=-1)throw std::runtime_error("duplicate previous pair");
   slot=std::stod(columns[8]);if(!finite_count(slot))throw std::runtime_error("invalid previous strength");++read;
  }
  if(!prev.eof()||read!=pair_count)throw std::runtime_error("incomplete previous pairs");
 }
 const std::string definition="For each distinct experience pair, does a recorded common member exist? Evidence is indexed from all incoming experiences with original source addresses; absent members are not refutations. Current member observations are model-qualified tags; no image-identity or DINO-similarity truth is inferred.";
 auto definition_address=graph.append("{\"definition\":"+std::string(quote_json(definition,scratch))+"}");
 heads<<"{\"definition\":";address(heads,definition_address);heads<<"}\n";
 edges<<"left,right,support,refute,observations,status,reason,previous_strength,strength,seed\n";
 std::uint64_t statuses[3]{},completed=0,total_support=0;
 std::ostringstream chunk;chunk<<std::setprecision(17);std::size_t chunk_count=0;
 auto flush=[&]{if(!chunk_count)return;auto p=graph.append("{\"definition_record\":\""+hex(definition_address.digest)+"\",\"links\":["+chunk.str()+"]}");heads<<"{\"links\":"<<chunk_count<<",\"record\":";address(heads,p);heads<<"}\n";chunk.str("");chunk.clear();chunk_count=0;};
 for_each_experience_pair(original_addresses,shuffle_seed,[&](const ExperiencePair& pair){
  const auto a=pair.left,b=pair.right;const auto& left=inputs[a];const auto& right=inputs[b];
  std::vector<std::uint32_t> common;
  std::set_intersection(left.tags.begin(),left.tags.end(),right.tags.begin(),right.tags.end(),std::back_inserter(common));
  AssociationEvidence evidence;std::uint64_t external_support=0;
  for(auto member:common){
   // Fresh measurement and judgment per EXPERIENCE pair. Global entries below
   // are raw membership observations, never another pair's approval.
   if(observe_common_member(left.binding,left.tags,right.binding,right.tags,member)!=EvidenceOutcome::support)continue;
   const auto& witnesses=tags[member].witnesses;
   evidence.support+=witnesses.size();
   external_support+=witnesses.size()-std::binary_search(witnesses.begin(),witnesses.end(),a)-std::binary_search(witnesses.begin(),witnesses.end(),b);
  }
  const double previous=fresh?1.0:prior_weights.at(ordinal(a,b));
  const auto judgment=judge_association(evidence);const auto strength=revise_association_strength(previous,judgment);
  if(!strength.valid())throw std::runtime_error("invalid pair strength");
  const auto status=static_cast<unsigned>(judgment.status()),reason=static_cast<unsigned>(judgment.reason());
  ++statuses[status];total_support+=evidence.support;
  edges<<a<<','<<b<<','<<evidence.support<<",0,"<<evidence.support<<','<<status<<','<<reason<<','<<previous<<','<<strength.current()<<','<<shuffle_seed<<'\n';
  if(chunk_count++)chunk<<',';
  chunk<<"{\"left\":"<<a<<",\"right\":"<<b<<",\"support\":"<<evidence.support<<",\"external_support\":"<<external_support<<",\"refute\":0,\"status\":"<<status<<",\"reason\":"<<reason<<",\"previous_strength\":"<<previous<<",\"strength\":"<<strength.current()<<",\"common_members\":[";
  for(std::size_t k=0;k<common.size();++k){if(k)chunk<<',';chunk<<common[k];}
  chunk<<"]}";
  if(chunk_count==512)flush();
  if(++completed%25000==0)std::cout<<"experience_pairs="<<completed<<'/'<<pair_count<<" accept="<<statuses[1]<<" reject="<<statuses[2]<<" abstain="<<statuses[0]<<std::endl;
 });
 flush();edges.close();heads.close();sources.close();nodes.close();aliases.close();
 auto seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
 std::ofstream summary(out/"summary.json");summary.exceptions(std::ios::badbit|std::ios::failbit);
 summary<<"{\"schema\":\"experience_pairs_common_member_v1\",\"images\":"<<count<<",\"input_occurrences\":"<<input_occurrences<<",\"duplicate_occurrences\":"<<duplicate_occurrences<<",\"tags\":"<<tags.size()<<",\"pairs\":"<<completed<<",\"accept\":"<<statuses[1]<<",\"reject\":"<<statuses[2]<<",\"abstain\":"<<statuses[0]<<",\"support_observations\":"<<total_support<<",\"verified_input_bindings\":"<<verified_bindings<<",\"shuffle_seed\":"<<shuffle_seed<<",\"seconds\":"<<seconds<<",\"main_merged\":false}\n";
 summary.close();std::cout<<"FINISHED experience_pairs="<<completed<<" accept="<<statuses[1]<<" reject="<<statuses[2]<<" abstain="<<statuses[0]<<std::endl;
}catch(const std::exception& e){std::cerr<<"FAILED: "<<e.what()<<std::endl;return 1;}
