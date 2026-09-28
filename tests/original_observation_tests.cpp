#include "vrs/original_observation.hpp"
#include "vrs/connection.hpp"
#include "swegca_architecture/evidence_rules.hpp"
#include <cstdlib>
#include <iostream>
#include <unistd.h>
using namespace swegca::vrs;
using namespace swegca::architecture;
using namespace swegca::architecture::kernel;
void check(bool ok){if(!ok)throw std::runtime_error("check failed");}
DigestBytes id(unsigned n){DigestBytes x{};x[0]=std::byte(n);return x;}
int main()try{
 std::string temp="/var/tmp/vrs-original-observation-XXXXXX";
 if(!mkdtemp(temp.data()))throw std::runtime_error("temporary directory");
 MemoryBudget memory(16<<20);auto rules=make_evidence_rules(EvidencePolicy{});
 auto block=ExperienceBlock::create(std::filesystem::path(temp)/"originals.block",id(1),1<<20);
 EvidenceObservation value;value.hypothesis=id(2);value.source=id(3);value.context=id(4);value.producer=id(5);value.producer_confidence=1;
 const std::string a("a\0b",3),b("a\0c",3);
 auto save=[&](std::uint64_t seq,const std::string& bytes){return record_evidence(block,rules,{seq,0,"original-observation-test","recorded-source","application/octet-stream",std::as_bytes(std::span(bytes))},value);};
 auto ea=save(1,a),eb=save(2,a),ec=save(3,b);
 auto sa=block.read(ea.original(),1<<20,memory),sb=block.read(eb.original(),1<<20,memory),sc=block.read(ec.original(),1<<20,memory);
 const auto same=observe_original_content_relation(sa,rules,sb,rules);
 check(same.outcome==EvidenceOutcome::support&&same.left==ea.original()&&same.right==eb.original());
 check(same.left_provenance.outcome==EvidenceOutcome::insufficient&&same.right_provenance.outcome==EvidenceOutcome::insufficient);
 check(same.left_provenance.source==value.source&&same.left_bytes==3&&same.right_bytes==3);
 check(observe_original_content_relation(sa,rules,sc,rules).outcome==EvidenceOutcome::refute);
 check(observe_original_content_relation(sa,rules,sc,rules,false).outcome==EvidenceOutcome::support);
 bool rejected=false;try{(void)observe_original_content_relation(sa,rules,sa,rules);}catch(const std::invalid_argument&){rejected=true;}check(rejected);
 // Feed measured outcome to the real record/admission/shuffle/core path.
 // One observed match still cannot satisfy the unchanged default policy.
 value.outcome=same.outcome;
 const std::string proof="test-derived relation; originals retained in test owner";
 auto measured=record_evidence(block,rules,{4,0,"original-observation-test","recorded-source","application/json",std::as_bytes(std::span(proof))},value);
 Connection connection(id(2),1.0,rules,memory);connection.append(measured);
 auto result=connection.refine(1703,0);
 check(result.result().verification().judgment().status()==EvidenceStatus::abstain);
 check(result.result().strength().current()==1.0);
 std::cout<<"original payload evidence checks passed; measured support remains core abstain under default policy\n";
 std::filesystem::remove_all(temp);
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
