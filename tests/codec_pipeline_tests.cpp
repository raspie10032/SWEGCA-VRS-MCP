#include "vrs/work_pipeline.hpp"
#include "vrs/codec_input.hpp"
#include "vrs/device_evidence.hpp"
#include "swegca_architecture/evidence_rules.hpp"
#include "swegca_architecture/ternary_count_kernel.hpp"
#include <fstream>
#include <set>
#include <cassert>
#include <iostream>
#include <unistd.h>
using namespace swegca::vrs;using namespace swegca::architecture;using namespace swegca::architecture::kernel;
struct Result {std::shared_ptr<const CodecInput> input;EvidenceJudgment judgment;};
// Integration fixture: deliberately UNKNOWN semantic evidence. This checks
// transport/order/ownership without inventing claims from pixels or bytes.
int main(){
 char dir[]="/tmp/swegca-codec-flow-XXXXXX";assert(mkdtemp(dir));std::filesystem::path root(dir);
 {std::ofstream f(root/"image.ppm",std::ios::binary);f<<"P6\n1 1\n255\n";const char bytes[]={char(255),0,0};f.write(bytes,3);}
 MemoryBudget memory(64ULL<<20);const auto rules=make_evidence_rules({});bool applied=false;
 using Flow=WorkPipeline<std::filesystem::path,CodecInput,Result>;
 Flow flow({2,8,8,8,8,std::chrono::milliseconds(5)},[&](auto p){return decode_file(p,memory);},{{"SWEGCA CPU",1,64,[&](auto batch){
  assert(!std::filesystem::exists(root/"applied"));std::vector<EvidenceTally> evidence(batch.size());std::vector<EvidenceJudgment> judgments(batch.size());cpu_evidence_batch(rules,evidence,judgments);
  std::vector<Result> results;for(size_t i=0;i<batch.size();++i){assert(!batch[i]->decoded.empty());results.push_back({batch[i],judgments[i]});}return results;
 }}},[&](auto batch){
  for(const auto& r:batch){assert(r.judgment.status()==EvidenceStatus::abstain);assert(!r.input->decoded.empty());TernaryCount count;assert(accumulate_verdict(count,r.judgment.status()));assert(count.abstain==1);
   // The first persistence in this fixture happens only in this callback.
   std::ofstream out(root/"applied",std::ios::binary);out.write(reinterpret_cast<const char*>(r.input->decoded[0].bytes.data()),r.input->decoded[0].bytes.size());applied=true;
  }
 });
 flow.submit(root/"image.ppm");flow.finish();assert(applied&&flow.stats().applied==1&&std::filesystem::file_size(root/"applied")>0);
 std::cout<<"PASS real file -> real codec -> SWEGCA -> single apply; no persistence before judgment; NOT semantic synapse evidence\n";
 std::filesystem::remove_all(root);
}
