#include "vrs/experience_pairs.hpp"
#include "swegca_architecture/association_kernel.hpp"
#include <array>
#include <iostream>
#include <map>
#include <set>
using namespace swegca::vrs;
using namespace swegca::architecture;
using namespace swegca::architecture::kernel;
void require(bool ok){if(!ok)throw std::runtime_error("pair contract failed");}
int main(){
 std::array<RecordAddress,4> input{};
 for(unsigned i=0;i<input.size();++i){input[i].offset=i;input[i].bytes=1;input[i].digest[0]=std::byte(i+1);}
 using Pair=std::pair<std::size_t,std::size_t>;
 std::set<Pair> seen;std::map<Pair,EvidenceStatus> verdicts;
 auto n=for_each_experience_pair(input,11,[&](const ExperiencePair& p){
  require(p.left<p.right);require(p.all_inputs.data()==input.data());require(p.all_inputs.size()==4);
  require(seen.emplace(p.left,p.right).second);
  AssociationEvidence evidence;
  // Synthetic observations test wiring ONLY, not an image predicate.
  // Third experience is available to support (0,1); no pair-wide verdict
  // or evidence is copied to the other five independent verification calls.
  if(p.left==0&&p.right==1&&p.all_inputs[2].digest[0]==std::byte{3})evidence.support=1;
  if(p.left==0&&p.right==2)evidence.refute=1;
  verdicts[{p.left,p.right}]=judge_association(evidence).status();
 });
 require(n==6&&seen.size()==6);
 require(verdicts.at({0,1})==EvidenceStatus::accept);
 require(verdicts.at({0,2})==EvidenceStatus::reject);
 require(verdicts.at({1,2})==EvidenceStatus::abstain);
 auto expected=seen;seen.clear();
 for_each_experience_pair(input,29,[&](const ExperiencePair& p){seen.emplace(p.left,p.right);});
 require(seen==expected);
 require(for_each_experience_pair(std::span<const RecordAddress>{},1,[](auto){throw std::runtime_error("empty");})==0);
 require(for_each_experience_pair(std::span(input).first(1),1,[](auto){throw std::runtime_error("self");})==0);
 bool stopped=false;unsigned calls=0;
 try{for_each_experience_pair(input,1,[&](auto){++calls;throw std::runtime_error("stop");});}catch(const std::runtime_error&){stopped=true;}
 require(stopped&&calls==1);
 std::cout<<"PASS: six distinct experience pairs; full-input evidence; independent core verdicts; shuffle coverage; no self-pairs; stop propagation\n";
}
