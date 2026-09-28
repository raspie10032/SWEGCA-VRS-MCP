#include "vrs/synapse.hpp"
#include "swegca_architecture/sha256.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>

namespace swegca::vrs {
namespace {
using namespace architecture;
using namespace architecture::kernel;
constexpr std::string_view magic = "SWGCSYN1";
constexpr std::string_view media = "application/vnd.swegca.synapse-binding-v1";
constexpr std::string_view source = "swegca-synapse";
constexpr std::size_t header = 96, address_size = 80;
void put(std::span<std::byte> b, std::size_t at, std::uint64_t x) {
    for(unsigned i=0;i<8;++i)b[at+i]=std::byte(x>>(8*i));
}
std::uint64_t get(std::span<const std::byte> b,std::size_t at) {
    std::uint64_t x=0;
    for(unsigned i=0;i<8;++i)x|=std::uint64_t(std::to_integer<unsigned>(b[at+i]))<<(8*i);
    return x;
}
void encode(std::span<std::byte> b,std::size_t at,const ExperienceLocation& p) {
    std::copy(p.block.begin(),p.block.end(),b.begin()+at);
    put(b,at+32,p.offset);put(b,at+40,p.bytes);
    std::copy(p.digest.begin(),p.digest.end(),b.begin()+at+48);
}
ExperienceLocation decode(std::span<const std::byte> b,std::size_t at) {
    ExperienceLocation p;
    std::copy_n(b.begin()+at,32,p.block.begin());
    p.offset=get(b,at+32);p.bytes=get(b,at+40);
    std::copy_n(b.begin()+at+48,32,p.digest.begin());
    return p;
}
void authenticate(SessionStore& session,std::span<const ExperienceLocation> members,
    const ExperienceLocation& definition,MemoryBudget& memory,std::uint64_t limit) {
    if(members.size()<2)throw std::invalid_argument("synapse requires at least two members");
    // Address equality uses all fields. Retain member order in the binding.
    std::pmr::set<std::array<std::byte,address_size>> seen(&memory);
    for(const auto& member:members){
        std::array<std::byte,address_size> key{};encode(key,0,member);
        if(!seen.insert(key).second)throw std::invalid_argument("duplicate synapse member");
        (void)session.read(member,limit);
    }
    const auto criterion=session.read(definition,limit);
    if(criterion.view().content.empty())throw std::invalid_argument("empty synapse definition");
}
}

Synapse::Binding Synapse::write_binding(SessionStore& session,
    std::span<const ExperienceLocation> members,const ExperienceLocation& definition,
    MemoryBudget& memory,std::uint64_t limit) {
    auto available=limit;
    for(const auto bytes:{std::size_t(ExperienceBlock::record_overhead),session.name().size(),source.size(),media.size(),header}){
        if(bytes>available)throw std::length_error("synapse binding exceeds read limit");
        available-=bytes;
    }
    if(members.size()>available/address_size ||
       members.size()>(std::numeric_limits<std::size_t>::max()-header)/address_size)
        throw std::length_error("synapse binding exceeds read limit");
    authenticate(session,members,definition,memory,limit);
    Binding binding(memory);
    binding.members.assign(members.begin(),members.end());binding.definition=definition;
    std::pmr::vector<std::byte> encoded(header+members.size()*address_size,&memory);
    for(std::size_t i=0;i<magic.size();++i)encoded[i]=std::byte(magic[i]);
    put(encoded,8,members.size());encode(encoded,16,definition);
    for(std::size_t i=0;i<members.size();++i)encode(encoded,header+i*address_size,members[i]);
    binding.identity=Sha256::of(encoded);
    binding.record=session.append({session.original_count(),0,session.name(),source,media,encoded});
    return binding;
}
Synapse::Binding Synapse::read_binding(SessionStore& session,const ExperienceLocation& address,
    MemoryBudget& memory,std::uint64_t limit) {
    const auto saved=session.read(address,limit);
    const auto view=saved.view();const auto b=view.content;
    if(view.media_type!=media || view.source!=source || b.size()<header ||
       !std::equal(magic.begin(),magic.end(),b.begin(),[](char a,std::byte v){return std::byte(a)==v;}))
        throw std::runtime_error("invalid synapse binding");
    const auto count=get(b,8);
    if(count<2 || count!=(b.size()-header)/address_size || (b.size()-header)%address_size)
        throw std::runtime_error("invalid synapse member count");
    Binding binding(memory);binding.record=address;binding.definition=decode(b,16);
    binding.identity=Sha256::of(b);binding.members.reserve(count);
    for(std::size_t i=0;i<count;++i)binding.members.push_back(decode(b,header+i*address_size));
    authenticate(session,binding.members,binding.definition,memory,limit);
    return binding;
}
Synapse Synapse::create(SessionStore& session,std::span<const ExperienceLocation> members,
    const ExperienceLocation& definition,double strength,const architecture::EvidencePolicy& policy,
    MemoryBudget& memory,std::uint64_t limit) {
    (void)architecture::make_evidence_rules(policy);
    if(!architecture::kernel::finite_count(strength))throw std::invalid_argument("invalid initial strength");
    return Synapse(session,write_binding(session,members,definition,memory,limit),strength,policy,memory,limit);
}
Synapse Synapse::recover(SessionStore& session,const SynapseCheckpoint& checkpoint,
    MemoryBudget& memory,std::uint64_t limit) {
    return Synapse(session,read_binding(session,checkpoint.binding,memory,limit),checkpoint.state,memory,limit);
}
Synapse::Synapse(SessionStore& session,Binding binding,double strength,
    const architecture::EvidencePolicy& policy,MemoryBudget& memory,std::uint64_t limit)
    :session_(session),binding_(std::move(binding)),
     connection_(PersistentConnection::create(session,binding_.identity,strength,policy,memory,limit)) {}
Synapse::Synapse(SessionStore& session,Binding binding,const ExperienceLocation& head,
    MemoryBudget& memory,std::uint64_t limit)
    :session_(session),binding_(std::move(binding)),
     connection_(PersistentConnection::recover(session,head,memory,limit)) {
    if(connection_.state().identity()!=binding_.identity)
        throw std::runtime_error("synapse binding and state mismatch");
}
ExperienceLocation Synapse::observe(const OriginalExperienceView& original,
    const architecture::kernel::EvidenceObservation& observation) {
    if(observation.hypothesis!=identity())throw std::invalid_argument("observation belongs to another synapse");
    const auto saved=session_.append_evidence(rules(),original,observation);
    connection_.append(saved.original());
    return saved.original();
}
void Synapse::append(const ExperienceLocation& original) { connection_.append(original); }
ConnectionRefinement Synapse::refine(std::uint64_t seed,std::uint64_t step) {
    return connection_.refine(seed,step);
}
} // namespace swegca::vrs
