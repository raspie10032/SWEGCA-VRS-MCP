#pragma once
#include "swegca_architecture/connection_growth_kernel.hpp"
#include <set>
#include <stdexcept>
#include <ostream>
#include <iomanip>
namespace swegca::vrs {
// One concept, one explicit VRS round/window. Keys are original experience
// identities, shared across blocks; block IDs and delivery counts are not keys.
class ConceptGrowth final {
public:
 using Id=architecture::kernel::Digest;
 void observe(const Id& experience,const architecture::kernel::TernaryCount& before,
              const architecture::kernel::TernaryCount& after){
  const auto value=architecture::kernel::connection_growth(before,after);
  if(value.state==architecture::kernel::GrowthState::invalid_counts)throw std::invalid_argument("invalid concept growth window");
  if(value.state==architecture::kernel::GrowthState::no_observation)return;
  auto combined=delta_;if(!architecture::kernel::merge_growth_counts(combined,value.delta)||
      architecture::kernel::connection_growth({},combined).state==architecture::kernel::GrowthState::invalid_counts)
   throw std::overflow_error("concept growth count overflow");
  tested_.insert(experience);if(value.delta.accept)supported_.insert(experience);delta_=combined;
 }
 void merge(const ConceptGrowth& other){
  auto combined=delta_;if(!architecture::kernel::merge_growth_counts(combined,other.delta_)||
   architecture::kernel::connection_growth({},combined).state==architecture::kernel::GrowthState::invalid_counts)throw std::overflow_error("concept growth count overflow");
  tested_.insert(other.tested_.begin(),other.tested_.end());supported_.insert(other.supported_.begin(),other.supported_.end());delta_=combined;
 }
 auto growth()const noexcept{return architecture::kernel::connection_growth({},delta_);}
 auto spread(std::uint64_t distinct_population)const noexcept{return architecture::kernel::experience_spread(distinct_population,tested_.size(),supported_.size());}
 // Counts and window identity are the reproducible evidence. A zero-count
 // window is encoded with null fractions, not invented zero-confidence data.
 void write(std::ostream& out,std::uint64_t window,std::uint64_t concept_id,std::uint64_t population)const{
  const auto g=growth();const auto s=spread(population);if(!s.valid)throw std::invalid_argument("concept population mismatch");
  const auto precision=out.precision();out<<std::setprecision(17);
  out<<"{\"window\":"<<window<<",\"concept\":"<<concept_id<<",\"scope\":\"direct_input_membership\",\"delta\":["<<g.delta.accept<<','<<g.delta.reject<<','<<g.delta.abstain<<"],\"N\":"<<g.observations<<",\"state\":\""<<(g.state==architecture::kernel::GrowthState::observed?"observed":"no_observation")<<"\",\"fractions\":";
  if(g.state==architecture::kernel::GrowthState::observed)out<<'['<<g.accept_fraction<<','<<g.refute_fraction<<','<<g.abstain_fraction<<']';else out<<"null";
  out<<",\"distinct_population\":"<<s.population<<",\"distinct_tested\":"<<s.tested<<",\"distinct_supported\":"<<s.supported<<",\"tested_fraction\":";
  if(s.population)out<<s.tested_fraction;else out<<"null";
  out<<",\"supported_fraction\":";if(s.population)out<<s.supported_fraction;else out<<"null";
  out<<",\"promotion_decision\":null}\n";out.precision(precision);
 }
private:
 architecture::kernel::TernaryCount delta_{};
 std::set<Id> tested_,supported_;
};
}
