#include "vrs/experience_page.hpp"
#include "swegca_architecture/metadata_residency_kernel.hpp"
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <bit>
#include <stdexcept>

namespace swegca::vrs {
namespace {
constexpr std::size_t header=16,entry=272;
constexpr std::string_view magic="SWGCEPG1",session="swegca-derived-metadata",
    source="sealed-experience-page",media="application/vnd.swegca.experience-page-v1";
void put(std::span<std::byte> bytes,std::size_t offset,std::uint64_t value,unsigned count=8){
    for(unsigned n=0;n<count;++n)bytes[offset+n]=std::byte((value>>(8*n))&255);
}
std::uint64_t get(std::span<const std::byte> bytes,std::size_t offset,unsigned count=8){
    std::uint64_t value=0;
    for(unsigned n=0;n<count;++n)value|=std::uint64_t(std::to_integer<unsigned>(bytes[offset+n]))<<(8*n);
    return value;
}
void digest(std::span<std::byte> bytes,std::size_t offset,const architecture::DigestBytes& value){
    std::copy(value.begin(),value.end(),bytes.begin()+offset);
}
architecture::DigestBytes digest(std::span<const std::byte> bytes,std::size_t offset){
    architecture::DigestBytes value;std::copy_n(bytes.begin()+offset,value.size(),value.begin());return value;
}
}
ExperiencePage::ExperiencePage(ExperiencePage&& other) noexcept
    :block_(std::move(other.block_)),location_(other.location_),count_(other.count_),path_(std::move(other.path_)),charge_(std::move(other.charge_)){}
ExperiencePage& ExperiencePage::operator=(ExperiencePage&& other) noexcept {
    if(this!=&other){
        discard();block_=std::move(other.block_);location_=other.location_;
        count_=other.count_;path_=std::move(other.path_);charge_=std::move(other.charge_);
    }
    return *this;
}
ExperiencePage::~ExperiencePage(){discard();}
void ExperiencePage::discard() noexcept {
    if(block_.fd_<0)return;
    struct stat owned{},named{};
    if(::fstat(block_.fd_,&owned)<0||::lstat(path_.c_str(),&named)<0)return;
    const bool same=S_ISREG(named.st_mode)&&owned.st_dev==named.st_dev&&owned.st_ino==named.st_ino&&
        owned.st_size>=0&&static_cast<std::uint64_t>(owned.st_size)==block_.end_;
    if(!architecture::kernel::discard_metadata_page(same,owned.st_nlink==1)||owned.st_size<0)return;
    // Runtime owns the directory exclusively. A replaced name, hard link or
    // failed unlink remains conservatively charged for cold reconciliation.
    if(::unlink(path_.c_str())<0)return;
    ::close(block_.fd_);block_.fd_=-1;
    if(charge_)StorageBudget::reclaim_removed(charge_,static_cast<std::uint64_t>(owned.st_size));
}
ExperiencePage ExperiencePage::create(const std::filesystem::path& path,
    const architecture::DigestBytes& identity,std::span<const ExperienceEvidence> values,
    MemoryBudget& memory,StorageBudget* storage){
    if(values.empty()||values.size()>capacity)throw std::invalid_argument("experience page size");
    std::pmr::vector<std::byte> bytes(header+entry*values.size(),&memory);
    for(std::size_t n=0;n<magic.size();++n)bytes[n]=std::byte(magic[n]);
    put(bytes,8,values.size());
    for(std::size_t n=0;n<values.size();++n){
        auto out=std::span(bytes).subspan(header+n*entry,entry);
        const auto& sealed=values[n];const auto& original=sealed.original();const auto& value=sealed.value();
        if(value.address!=original.digest)throw std::logic_error("unbound sealed metadata");
        digest(out,0,original.block);put(out,32,original.offset);put(out,40,original.bytes);digest(out,48,original.digest);
        digest(out,80,value.hypothesis);digest(out,112,value.source);digest(out,144,value.context);digest(out,176,value.producer);
        put(out,208,value.observed_at);put(out,216,value.expires_at);
        put(out,224,std::bit_cast<std::uint64_t>(value.producer_confidence));put(out,232,value.axis,4);
        put(out,236,static_cast<unsigned>(value.outcome),1);put(out,237,value.has_expiry,1);
        put(out,238,sealed.has_input_key(),1);digest(out,240,sealed.cue());
    }
    auto owned_path=path; // allocate before creating the file
    auto block=ExperienceBlock::create(path,identity,ExperienceBlock::header_bytes+
        ExperienceBlock::record_overhead+session.size()+source.size()+media.size()+bytes.size(),storage);
    // Own the private header before append can reject its reservation. The
    // same inode/sole-link/core disposal checks apply during stack unwinding.
    // Partial writes with an unexpected extent remain conservatively retained.
    ExperiencePage page(std::move(block),{},values.size(),std::move(owned_path),storage);
    page.location_=page.block_.append({0,0,session,source,media,bytes});
    return page;
}
ExperienceEvidence ExperiencePage::read(std::size_t index,
    const architecture::kernel::EvidenceRules& rules,MemoryBudget& memory) const {
    if(index>=count_)throw std::out_of_range("experience page index");
    auto stored=block_.read(location_,location_.bytes,memory);
    return decode(index,rules,stored);
}
std::pmr::vector<ExperienceEvidence> ExperiencePage::load(
    const architecture::kernel::EvidenceRules& rules,MemoryBudget& memory) const {
    auto stored=block_.read(location_,location_.bytes,memory);
    std::pmr::vector<ExperienceEvidence> values(&memory);values.reserve(count_);
    for(std::size_t n=0;n<count_;++n)values.push_back(decode(n,rules,stored));
    return values;
}
void ExperiencePage::restore_into(ExperienceEvidence* output,std::size_t count,
    const architecture::kernel::EvidenceRules& rules,MemoryBudget& memory) const {
    if(count!=count_)throw std::runtime_error("experience page count changed");
    auto stored=block_.read(location_,location_.bytes,memory);
    std::size_t constructed=0;
    try {
        for(;constructed<count;++constructed)
            std::construct_at(output+constructed,decode(constructed,rules,stored));
    } catch(...) {
        std::destroy_n(output,constructed);
        throw;
    }
}
ExperienceEvidence ExperiencePage::decode(std::size_t index,
    const architecture::kernel::EvidenceRules& rules,const StoredExperience& stored) const {
    using namespace architecture::kernel;
    const auto view=stored.view();const auto bytes=view.content;
    if(view.session!=session||view.source!=source||view.media_type!=media||view.sequence||view.observed_at_ns||
       bytes.size()!=header+entry*count_||get(bytes,8)!=count_||
       !std::equal(magic.begin(),magic.end(),bytes.begin(),[](char a,std::byte b){return std::byte(a)==b;}))
        throw std::runtime_error("invalid experience page metadata");
    const auto data=bytes.subspan(header+index*entry,entry);
    if(get(data,237,1)>1||get(data,238,1)>1||get(data,239,1))
        throw std::runtime_error("invalid experience page flags");
    const ExperienceLocation original{digest(data,0),get(data,32),get(data,40),digest(data,48)};
    EvidenceObservation value;
    value.address=original.digest;value.hypothesis=digest(data,80);value.source=digest(data,112);
    value.context=digest(data,144);value.producer=digest(data,176);value.observed_at=get(data,208);
    value.expires_at=get(data,216);value.producer_confidence=std::bit_cast<double>(get(data,224));
    value.axis=static_cast<std::uint32_t>(get(data,232,4));
    value.outcome=static_cast<EvidenceOutcome>(get(data,236,1));value.has_expiry=get(data,237,1)!=0;
    if(admit_observation(rules,value.hypothesis,value,value.observed_at,false)==ObservationUse::invalid)
        throw std::runtime_error("invalid paged observation");
    return ExperienceEvidence(original,value,digest(data,240),get(data,238,1)!=0);
}
} // namespace swegca::vrs
