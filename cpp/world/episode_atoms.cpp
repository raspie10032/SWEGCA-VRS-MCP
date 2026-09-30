#include "world/episode_atoms.hpp"

#include "swegca_architecture/sha256.hpp"
#include "world/semantic_vrs_ingress.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>

namespace swegca::world {
namespace {
using architecture::Sha256;

std::string hex(const architecture::DigestBytes& digest) {
    constexpr char digits[]="0123456789abcdef"; std::string out(64,'0');
    for(std::size_t i=0;i<digest.size();++i){auto v=std::to_integer<unsigned>(digest[i]);out[2*i]=digits[v>>4U];out[2*i+1]=digits[v&15U];} return out;
}
JsonValue strings(const std::vector<std::string>& values){JsonValue::Array out;for(const auto& v:values)out.emplace_back(v);return out;}
std::vector<std::byte> step_bytes(const MemoryStep& step) {
    JsonValue value=JsonValue::Object{{"evidence_refs",strings(step.evidence_refs)},
        {"judgment",step.judgment},{"observation",step.observation},{"outcome",step.outcome},
        {"phase",step.phase},{"relations",strings(step.relations)}};
    const auto wire=semantic_canonical_json(value);
    return {reinterpret_cast<const std::byte*>(wire.data()),reinterpret_cast<const std::byte*>(wire.data()+wire.size())};
}
std::vector<std::string> provenance(const MemoryEpisode& episode,const MemoryStep& step){
    auto out=episode.source_addresses;for(const auto& ref:step.evidence_refs)if(std::ranges::find(out,ref)==out.end())out.push_back(ref);return out;
}
std::optional<TimePoint> observed_time(const MemoryStep& step){
    const auto found=step.observation.find("observed_at_unix_ns");if(found==step.observation.end())return std::nullopt;
    const auto* value=std::get_if<std::int64_t>(&found->second.storage());if(!value)throw std::invalid_argument("observed timestamp must be integer nanoseconds");
    return TimePoint("unix",*value);
}
}

EpisodeAtoms::EpisodeAtoms(const MemoryEpisode* episode_value,std::vector<AtomizedExperience> step_values,
    std::map<std::string,std::pair<std::size_t,std::size_t>,std::less<>> location_values)
    :episode(episode_value),steps(std::move(step_values)),locations(std::move(location_values)){
    if(!episode||steps.size()!=episode->steps.size())throw std::invalid_argument("atomization must include every historical step");
    std::map<std::string,std::pair<std::size_t,std::size_t>,std::less<>> expected;
    std::size_t count=0;for(std::size_t i=0;i<steps.size();++i)for(const auto& atom:steps[i].atoms){expected.emplace(atom.atom_id,std::pair{i,atom.index});++count;}
    if(expected!=locations||expected.size()!=count)throw std::invalid_argument("atom address map changed or aliases collide");
}

EpisodeAtoms EpisodeAtoms::build(const MemoryEpisode& episode,const std::size_t maximum_bytes,const LosslessBlockCodec codec){
    std::vector<AtomizedExperience> steps;std::map<std::string,std::pair<std::size_t,std::size_t>,std::less<>> locations;
    for(std::size_t index=0;index<episode.steps.size();++index){const auto raw=step_bytes(episode.steps[index]);const auto hash=hex(Sha256::of(raw));
        AtomParent parent(episode.episode_id,"memory-step-artifact:"+std::to_string(index)+":sha256:"+hash,episode.revision,
            provenance(episode,episode.steps[index]),episode.steps[index].outcome,"record",observed_time(episode.steps[index]),"retained_derived_artifact");
        auto spans=text_spans(raw,maximum_bytes);auto bundle=AtomizedExperience::build(std::move(parent),raw,spans,codec);
        for(const auto& atom:bundle.atoms)if(!locations.emplace(atom.atom_id,std::pair{index,atom.index}).second)throw std::invalid_argument("duplicate atom address");
        steps.push_back(std::move(bundle));}
    return EpisodeAtoms(&episode,std::move(steps),std::move(locations));
}

void EpisodeAtoms::verify_cold() const{
    for(std::size_t i=0;i<steps.size();++i){const auto& bundle=steps[i];const auto raw=bundle.reconstruct_cold();const auto& step=episode->steps[i];
        if(raw!=step_bytes(step)||bundle.parent.parent_experience_id!=episode->episode_id||bundle.parent.revision!=episode->revision||
           bundle.parent.outcome!=step.outcome||bundle.parent.source_address!="memory-step-artifact:"+std::to_string(i)+":sha256:"+std::string(bundle.source_hash())||
           bundle.parent.provenance!=provenance(*episode,step)||bundle.parent.observed_at!=observed_time(step)||bundle.parent.modality!="record"||
           bundle.parent.representation!="retained_derived_artifact")throw std::invalid_argument("step atom lineage differs from original episode");}
}

std::pair<std::size_t,PreparedAtom> EpisodeAtoms::prepare(const std::string_view atom_id) const{
    const auto found=locations.find(atom_id);if(found==locations.end())throw std::out_of_range("unknown atom address");return {found->second.first,steps[found->second.first].prepare(found->second.second)};
}
const MemoryStep& PreparedEpisodeAtom::replay_step()const{return episode->steps.at(step_index);}

AtomRecallIndex::AtomRecallIndex(std::shared_ptr<const HotMemoryIndex> base_value,
    std::map<std::string,std::shared_ptr<const EpisodeAtoms>,std::less<>> binding_values)
    :base(std::move(base_value)),bindings(std::move(binding_values)){
    if(!base||base->lookup_requires_io())throw std::invalid_argument("hot parent memory required");
    for(const auto&[id,binding]:bindings){if(!binding||id!=binding->episode->episode_id||&base->episode(id)!=binding->episode)throw std::invalid_argument("invalid atom parent binding");
        binding->verify_cold();for(const auto&[atom,unused]:binding->locations){static_cast<void>(unused);if(!atom_parents_.emplace(atom,id).second)throw std::invalid_argument("duplicate atom address");}}
}
std::string_view AtomRecallIndex::snapshot_id()const noexcept{return base->snapshot_id();}
std::size_t AtomRecallIndex::episode_count()const noexcept{return base->episode_count();}
const std::map<std::string,std::size_t,std::less<>>& AtomRecallIndex::outcome_counts()const noexcept{return base->outcome_counts();}
const MemoryEpisode& AtomRecallIndex::episode(const std::string_view id)const{return base->episode(id);}
std::vector<std::string> AtomRecallIndex::episode_ids_for_cue(const std::string_view cue)const{return base->episode_ids_for_cue(cue);}
std::vector<std::string> AtomRecallIndex::iter_episode_ids()const{return base->iter_episode_ids();}
std::vector<std::string> AtomRecallIndex::atom_ids(const std::string_view id)const{base->episode(id);const auto found=bindings.find(id);if(found==bindings.end())throw AtomPreparationRequired(std::string(id));std::vector<std::string> out;for(const auto&[atom,unused]:found->second->locations){static_cast<void>(unused);out.push_back(atom);}return out;}
const MemoryEpisode& AtomRecallIndex::parent_for_atom(const std::string_view atom)const{return base->episode(atom_parents_.at(atom));}
PreparedEpisodeAtom AtomRecallIndex::prepare_atom(const std::string_view atom)const{const auto& id=atom_parents_.at(atom);const auto [step,item]=bindings.at(id)->prepare(atom);return {std::string(snapshot_id()),&base->episode(id),step,item};}

} // namespace swegca::world
