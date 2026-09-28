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
#include "vrs/input_collision.hpp"
#include "vrs/concept_growth.hpp"
#ifdef SWEGCA_PARALLEL_COLLISION
#include "vrs/collision_dispatch.hpp"
#endif
#include <chrono>
#include <tuple>
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
  OriginalExperienceView v{seq++,0,"input-collision","input-collision",media,content};auto need=ExperienceBlock::record_overhead+v.session.size()+v.source.size()+v.media_type.size()+content.size();
  if(!block||used+need>(64ULL<<20)){auto name="collision-"+std::to_string(index++)+".block";block.emplace(ExperienceBlock::create(root/name,key(root.string()+name),64ULL<<20,&storage));used=80;}
  auto a=block->append(v);used+=a.bytes;return a;
 }
};
std::uint64_t get64(std::span<const std::byte> bytes,std::size_t at){
 if(at>bytes.size()||bytes.size()-at<8)throw std::runtime_error("short matrix");
 std::uint64_t value=0;for(unsigned b=0;b<8;++b)value|=std::uint64_t(std::to_integer<unsigned>(bytes[at+b]))<<(8*b);return value;
}
void put64(std::vector<std::byte>& out,std::uint64_t n){for(unsigned b=0;b<8;++b)out.push_back(std::byte(n>>(8*b)));}
int main(int argc,char** argv)try{
 if(argc!=7&&argc!=8)throw std::runtime_error("usage: input-collision DATASET GRAPH TAG_IMAGE TAG_TAG OUTPUT ROUNDS [COUNT_CHECKPOINT]");
#ifndef SWEGCA_PARALLEL_COLLISION
 rlimit cap{4000000000ULL,4000000000ULL};if(setrlimit(RLIMIT_AS,&cap))throw std::runtime_error("memory limit");
#endif
 const std::filesystem::path data(argv[1]),graph(argv[2]),irpath(argv[3]),prpath(argv[4]),out(argv[5]);const unsigned rounds=std::stoul(argv[6]);
 if(!rounds||std::filesystem::exists(out/"rounds.jsonl"))throw std::runtime_error("invalid rounds/existing output");
 MemoryBudget mem{128ULL<<20};Reader gr(graph),ir(irpath),pr(prpath);
 auto meta=parse_json(load(irpath/"summary.json"),mem);const unsigned n=num(meta.at("images")),t=num(meta.at("tags"));
 if(meta.at("schema").string()!="binary_tag_image_v2"||!n||t<2)throw std::runtime_error("input schema/dimensions");
 std::vector<TernaryCount> ti(std::size_t(n)*t),tt(std::size_t(t)*(t-1)/2);
 std::map<unsigned,RecordAddress> source_records,tag_records,matrices;std::string line;
 {std::ifstream file(graph/"graph-records.jsonl");while(std::getline(file,line)){auto r=parse_json(line,mem);if(auto v=r.find("source"))source_records.emplace(num(*v),addr(r.at("record")));else if(auto v=r.find("tag"))tag_records.emplace(num(*v),addr(r.at("record")));}}
 {std::ifstream file(irpath/"tag-records.jsonl");while(std::getline(file,line)){auto r=parse_json(line,mem);if(auto v=r.find("tag"))matrices.emplace(num(*v),addr(r.at("record")));}}
 if(source_records.size()!=n||tag_records.size()!=t||matrices.size()!=t)throw std::runtime_error("missing source/tag/matrix");
 using TagKey=std::tuple<std::string,unsigned,unsigned,std::string>;
 std::map<TagKey,unsigned> dictionary;
 std::vector<std::string> tag_names(t);std::ofstream tag_names_log(out/"tag-dictionary.jsonl");
 for(auto [tag,a]:tag_records){auto r=gr.read(a);tag_names[tag]=std::string(r.at("tag").string());tag_names_log<<"{\"tag\":"<<tag<<",\"name\":";write_json_string(tag_names_log,tag_names[tag]);tag_names_log<<"}\n";dictionary.emplace(TagKey{std::string(r.at("model").string()),num(r.at("index")),num(r.at("category")),std::string(r.at("tag").string())},tag);}
 std::vector<CollisionInput> inputs(n);std::vector<std::string> image_ids(n);
 std::ofstream input_refs(out/"input-records.jsonl");input_refs.exceptions(std::ios::badbit|std::ios::failbit);
 for(auto [i,a]:source_records){if(i>=n)throw std::runtime_error("source id");auto source=gr.read(a);const auto fa=addr(source.at("feature_record"));auto feature=gr.read(fa);
  image_ids[i]=feature.at("sha256").string();const auto image=file_hash(data/"images"/(image_ids[i]+".bin"));const auto score_path=std::filesystem::path(feature.at("raw_scores").string());
  if(score_path.is_absolute())throw std::runtime_error("absolute score path");
  for(auto& part:score_path)if(part=="..")throw std::runtime_error("score parent path");
  const auto scores=file_hash(data/score_path);
  inputs[i].binding=bind_experience(image,digest(image_ids[i]),fa.digest,scores);
  const auto model=std::string(feature.at("models").at("tag_sha256").string());
  inputs[i].membership_observed=true;
  for(auto& r:feature.at("tags").values)inputs[i].members.push_back(dictionary.at(TagKey{model,num(r.at("index")),num(r.at("category")),std::string(r.at("tag").string())}));
  // Full sealed feature payload, raw scores and original image remain linked.
  // No use of prior source.tags/witnesses, semantic cleanliness or verdicts.
  input_refs<<"{\"input\":"<<i<<",\"source\":";address(input_refs,a);input_refs<<",\"feature\":";address(input_refs,fa);input_refs<<",\"actual_image_sha256\":\""<<hex(image)<<"\",\"raw_scores_sha256\":\""<<hex(scores)<<"\"}\n";
 }
 input_refs.close();
 for(auto [tag,a]:matrices){if(tag>=t)throw std::runtime_error("matrix id");auto stored=ir.blocks.at(a.block).read(a,2ULL<<20,ir.memory);auto b=stored.view().content;
  if(b.size()!=24+std::size_t(n)*16||get64(b,0)!=tag||get64(b,8)!=n||get64(b,16)!=2)throw std::runtime_error("matrix format");
  for(unsigned i=0;i<n;++i){auto& value=ti[std::size_t(tag)*n+i];const auto status=get64(b,24+i*16);if(status>2||!accumulate_verdict(value,static_cast<EvidenceStatus>(status)))throw std::runtime_error("initial verdict");}
 }
 {std::ifstream file(prpath/"pairs.jsonl");std::size_t pos=0;unsigned left=0,right=1;
  while(std::getline(file,line)){auto r=parse_json(line,mem);auto a=addr(r.at("record"));auto stored=pr.blocks.at(a.block).read(a,2ULL<<20,pr.memory);auto b=stored.view().content;
   if(b.size()!=std::size_t(num(r.at("pairs")))*40)throw std::runtime_error("pair shape");
   for(std::size_t at=0;at<b.size();at+=40){if(pos>=tt.size()||get64(b,at)!=left||get64(b,at+8)!=right)throw std::runtime_error("pair order");const auto status=get64(b,at+24);if(status>2||!accumulate_verdict(tt[pos++],static_cast<EvidenceStatus>(status)))throw std::runtime_error("initial pair verdict");if(++right==t){++left;right=left+1;}}
  }if(pos!=tt.size())throw std::runtime_error("missing pairs");
 }
 unsigned round_offset=0;
 if(argc==8){
  const std::filesystem::path base(argv[7]);auto previous=parse_json(load(base/"inputs.json"),mem);
  if(previous.at("representation").string()!="accept_reject_abstain_u64"||num(previous.at("images"))!=n||num(previous.at("tags"))!=t||previous.at("graph").string()!=graph.string()||previous.at("tag_image").string()!=irpath.string()||previous.at("tag_tag").string()!=prpath.string())throw std::runtime_error("checkpoint lineage mismatch");
  round_offset=num(previous.at("rounds"));Reader saved(base);std::size_t loaded_ti=0,loaded_tt=0;
  std::ifstream checkpoints(base/"checkpoints.jsonl");
  while(std::getline(checkpoints,line)){auto row=parse_json(line,mem);if(num(row.at("round"))!=round_offset)continue;
   const auto kind=row.at("kind").string();if(kind!="tag_image"&&kind!="tag_tag")throw std::runtime_error("checkpoint kind");
   auto& values=kind=="tag_image"?ti:tt;auto& loaded=kind=="tag_image"?loaded_ti:loaded_tt;
   const auto start=std::stoull(std::string(row.at("start").scalar)),count=std::stoull(std::string(row.at("count").scalar));
   if(start!=loaded||start>values.size()||count>values.size()-start)throw std::runtime_error("checkpoint range");
   const auto a=addr(row.at("record"));auto record=saved.blocks.at(a.block).read(a,2ULL<<20,saved.memory);const auto bytes=record.view().content;
   if(bytes.size()!=count*24)throw std::runtime_error("checkpoint count encoding");
   for(std::size_t i=0;i<count;++i)values[start+i]={get64(bytes,i*24),get64(bytes,i*24+8),get64(bytes,i*24+16)};
   loaded+=count;
  }
  if(loaded_ti!=ti.size()||loaded_tt!=tt.size())throw std::runtime_error("incomplete count checkpoint");
 }
 InputCollision::Dispatch dispatch;
#ifdef SWEGCA_PARALLEL_COLLISION
 CollisionDispatch pool((out/"dispatch").string());
 dispatch=[&](auto in,auto results,auto commit){pool.judge(in,results,std::move(commit));};
#endif
 InputCollision engine(n,t,ti,tt,std::move(dispatch));std::vector<unsigned> order(n);std::iota(order.begin(),order.end(),0);
 std::vector<TernaryCount> experience_counts(n);
 std::ofstream experience_log(out/"experience-counts.jsonl");experience_log.exceptions(std::ios::badbit|std::ios::failbit);
 std::ofstream growth_log(out/"concept-growth.jsonl");growth_log.exceptions(std::ios::badbit|std::ios::failbit);
 std::vector<DigestBytes> growth_ids;growth_ids.reserve(n);std::set<DigestBytes> growth_population;
 for(const auto& id:image_ids){auto d=digest(id);growth_ids.push_back(d);growth_population.insert(d);}
 Writer writer{out};std::ofstream records(out/"checkpoints.jsonl"),reports(out/"rounds.jsonl"),traces(out/"trace.jsonl"),concepts(out/"concepts.jsonl");
 for(auto* f:{&records,&reports,&traces,&concepts}){f->exceptions(std::ios::badbit|std::ios::failbit);*f<<std::setprecision(17);}
 auto checkpoint=[&](unsigned round,const char* kind,const std::vector<TernaryCount>& values){for(std::size_t start=0;start<values.size();start+=16384){auto span=std::span(values).subspan(start,std::min<std::size_t>(16384,values.size()-start));auto a=writer.append(std::as_bytes(span),"application/x-swegca-counts-u64x3");records<<"{\"round\":"<<round<<",\"kind\":\""<<kind<<"\",\"start\":"<<start<<",\"count\":"<<span.size()<<",\"record\":";address(records,a);records<<"}\n";}};
 // A resumed run already has an authenticated immutable baseline. Do not
 // rewrite it before verification; emit only the new completed checkpoint.
 if(!round_offset){checkpoint(0,"tag_image",ti);checkpoint(0,"tag_tag",tt);}
 for(unsigned round=round_offset+1;round<=round_offset+rounds;++round){
  const auto begin=std::chrono::steady_clock::now();const auto before_ti=ti,before_tt=tt;std::random_device entropy;const std::uint64_t seed=(std::uint64_t(entropy())<<32)^entropy();
  std::mt19937_64 rng(seed);std::shuffle(order.begin(),order.end(),rng);std::uint64_t counts[2][3]{},revived=0,fallen=0,transitions=0,logged=0;bool last=false,first=true;
  unsigned processed=0;
  for(unsigned image:order){engine.encounter(image,inputs[image],rng,[&](const CollisionEvent& e){
   if(!accumulate_verdict(experience_counts[image],e.status))throw std::overflow_error("experience count overflow");
   ++counts[e.member_pair][static_cast<unsigned>(e.status)];if(!first)transitions+=last!=e.member_pair;first=false;last=e.member_pair;
   const bool now=count_evidence_eligible(e.current);revived+=!e.was_active&&now;fallen+=e.was_active&&!now;
   if(logged<128){++logged;traces<<"{\"round\":"<<round<<",\"input\":"<<image<<",\"tag_pair\":"<<(e.member_pair?"true":"false")<<",\"left\":"<<e.left<<",\"right\":"<<e.right<<",\"status\":"<<unsigned(e.status)<<",\"previous\":"<<'['<<e.previous.accept<<','<<e.previous.reject<<','<<e.previous.abstain<<"],\"current\":["<<e.current.accept<<','<<e.current.reject<<','<<e.current.abstain<<']'<<"}\n";}
  });
   if(++processed%32==0||processed==n){
    auto tmp=out/"live.tmp";std::ofstream live(tmp);live<<"{\"phase\":\"actual_synapse_verification_and_application\",\"round\":"<<round<<",\"inputs_applied\":"<<processed<<",\"inputs_total\":"<<n<<",\"verified_applied\":"<<(counts[0][0]+counts[0][1]+counts[0][2]+counts[1][0]+counts[1][1]+counts[1][2])<<",\"accept\":"<<counts[0][1]+counts[1][1]<<",\"reject\":"<<counts[0][2]+counts[1][2]<<",\"abstain\":"<<counts[0][0]+counts[1][0]<<",\"checkpoint_complete\":false}\n";live.close();std::filesystem::rename(tmp,out/"live.json");
   }
  }
  for(unsigned i=0;i<n;++i){auto c=experience_counts[i];experience_log<<"{\"round\":"<<round<<",\"input\":"<<i<<",\"counts\":["<<c.accept<<','<<c.reject<<','<<c.abstain<<"]}\n";}experience_log.flush();
  std::uint64_t changed=0,active_ti=0,active_tt=0,active_changes=0,max_increment=0;
  const auto measure=[&](const std::vector<TernaryCount>& current,const std::vector<TernaryCount>& previous,std::uint64_t& active){for(std::size_t i=0;i<current.size();++i){
   const auto a=current[i],b=previous[i];changed+=a!=b;active+=count_evidence_eligible(a);active_changes+=count_evidence_eligible(a)!=count_evidence_eligible(b);
   max_increment=std::max({max_increment,a.accept-b.accept,a.reject-b.reject,a.abstain-b.abstain});
  }};
  measure(ti,before_ti,active_ti);measure(tt,before_tt,active_tt);unsigned common=0;
  for(unsigned tag=0;tag<t;++tag){std::set<std::string> distinct;ConceptGrowth growth;
   for(unsigned i=0;i<n;++i){const auto index=std::size_t(tag)*n+i;
    if(count_evidence_eligible(ti[index]))distinct.insert(image_ids[i]);
    growth.observe(growth_ids[i],before_ti[index],ti[index]);
   }
   growth.write(growth_log,round,tag,growth_population.size());
   common+=distinct.size()>1;if(round==round_offset+rounds)concepts<<"{\"tag\":"<<tag<<",\"distinct_images\":"<<distinct.size()<<",\"common_concept\":"<<(distinct.size()>1?"true":"false")<<"}\n";}
  {
   std::vector<TernaryCount> incident(t);std::vector<std::uint64_t> observed(t),positive(t),eligible(t);
   std::size_t index=0;
   for(unsigned left=0;left<t;++left)for(unsigned right=left+1;right<t;++right,++index){
    const auto g=connection_growth(before_tt[index],tt[index]);
    if(g.state==GrowthState::invalid_counts)throw std::runtime_error("invalid pair growth");
    for(auto tag:{left,right}){
     if(!merge_growth_counts(incident[tag],g.delta))throw std::overflow_error("incident count overflow");
     observed[tag]+=g.observations!=0;positive[tag]+=g.delta.accept>g.delta.reject;eligible[tag]+=count_evidence_eligible(tt[index]);
    }
   }
   std::ofstream pairs(out/"concept-pair-growth.jsonl",std::ios::app);
   for(unsigned tag=0;tag<t;++tag){const auto g=connection_growth({},incident[tag]);
    pairs<<"{\"round\":"<<round<<",\"tag\":"<<tag<<",\"scope\":\"incident_tag_pair\",\"delta\":["<<g.delta.accept<<','<<g.delta.reject<<','<<g.delta.abstain<<"],\"N\":"<<g.observations<<",\"observed_partner_edges\":"<<observed[tag]<<",\"positive_net_edges\":"<<positive[tag]<<",\"active_edges\":"<<eligible[tag]<<"}\n";
   }
  }
  growth_log.flush();
  checkpoint(round,"tag_image",ti);checkpoint(round,"tag_tag",tt);records.flush();
  reports<<"{\"round\":"<<round<<",\"seed\":"<<seed<<",\"input_order_sha256\":\""<<hex(Sha256::of(std::as_bytes(std::span(order))))<<"\",\"tag_image\":["<<counts[0][1]<<','<<counts[0][2]<<','<<counts[0][0]<<"],\"tag_tag\":["<<counts[1][1]<<','<<counts[1][2]<<','<<counts[1][0]<<"],\"type_switches\":"<<transitions<<",\"changed_strengths\":"<<changed<<",\"active_tag_image\":"<<active_ti<<",\"active_tag_tag\":"<<active_tt<<",\"active_set_changes\":"<<active_changes<<",\"revival_events\":"<<revived<<",\"deactivation_events\":"<<fallen<<",\"common_concepts\":"<<common<<",\"max_component_increment\":"<<max_increment<<",\"seconds\":"<<std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count()<<"}\n";reports.flush();
  std::cout<<"ROUND "<<round<<'/'<<round_offset+rounds<<" changed="<<changed<<" active_tag_tag="<<active_tt<<" revived="<<revived<<" fallen="<<fallen<<" max_increment="<<max_increment<<std::endl;
 }
 std::ofstream manifest(out/"inputs.json");manifest<<"{\"graph\":";write_json_string(manifest,graph.string());manifest<<",\"tag_image\":";write_json_string(manifest,irpath.string());manifest<<",\"tag_tag\":";write_json_string(manifest,prpath.string());manifest<<",\"rounds\":"<<round_offset+rounds<<",\"images\":"<<n<<",\"tags\":"<<t<<",\"representation\":\"accept_reject_abstain_u64\",\"activation\":\"accept>=reject\",\"initialization\":\"one recorded initial verdict per connection, no float inversion\",\"claim\":\"Does this incoming experience support this activated relation?\",\"main_merged\":false}\n";
}catch(const std::exception& e){std::cerr<<"FAILED: "<<e.what()<<std::endl;return 1;}
