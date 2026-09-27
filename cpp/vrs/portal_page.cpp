#include "vrs/portal_page.hpp"
#include "swegca_architecture/metadata_residency_kernel.hpp"
#include "swegca_architecture/region_partition_kernel.hpp"
#include <algorithm>
#include <sys/stat.h>
#include <unistd.h>
namespace swegca::vrs {
namespace {
constexpr std::size_t header=56,entry=48;
constexpr std::string_view magic="SWGCPRT1",session="swegca-derived-portals",
    source="main-portal-ranges",media="application/vnd.swegca.portal-page-v1";
void put(std::span<std::byte> bytes,std::size_t offset,std::uint64_t value){
    for(unsigned n=0;n<8;++n)bytes[offset+n]=std::byte((value>>(8*n))&255);
}
std::uint64_t get(std::span<const std::byte> bytes,std::size_t offset){
    std::uint64_t value=0;
    for(unsigned n=0;n<8;++n)value|=std::uint64_t(std::to_integer<unsigned>(bytes[offset+n]))<<(8*n);
    return value;
}
}
PortalPage::PortalPage(PortalPage&& other) noexcept
    :block_(std::move(other.block_)),location_(other.location_),kind_(other.kind_),lookup_(other.lookup_),
     count_(other.count_),path_(std::move(other.path_)),charge_(std::move(other.charge_)){}
PortalPage::~PortalPage(){discard();}
void PortalPage::discard() noexcept {
    if(block_.fd_<0)return;
    struct stat owned{},named{};
    if(::fstat(block_.fd_,&owned)<0||::lstat(path_.c_str(),&named)<0)return;
    const bool failed_append=location_.bytes==0&&!block_.writable_;
    const auto physical=owned.st_size<0?0:static_cast<std::uint64_t>(owned.st_size);
    const bool extent=failed_append?physical>=ExperienceBlock::header_bytes&&physical<=block_.capacity_:physical==block_.end_;
    const bool same=S_ISREG(named.st_mode)&&owned.st_dev==named.st_dev&&owned.st_ino==named.st_ino&&owned.st_size>=0&&extent;
    if(!architecture::kernel::discard_metadata_page(same,owned.st_nlink==1))return;
    if(::unlink(path_.c_str())<0)return;
    ::close(block_.fd_);block_.fd_=-1;
    if(charge_)StorageBudget::reclaim_removed(charge_,failed_append?block_.capacity_:physical);
}
PortalPage PortalPage::create(const std::filesystem::path& path,const architecture::DigestBytes& identity,
    Kind kind,const architecture::DigestBytes& lookup,std::span<const Range> ranges,MemoryBudget& memory,StorageBudget* storage){
    using namespace architecture::kernel;
    const auto partition=region_partition_end(0,ranges.size(),capacity);
    if((kind!=Kind::cue&&kind!=Kind::context)||!named_digest(lookup)||!partition||*partition!=ranges.size())
        throw std::invalid_argument("invalid portal page header");
    for(std::size_t n=0;n<ranges.size();++n)
        if(!portal_range_valid(ranges[n])||(n&&!portal_range_follows(ranges[n-1],ranges[n])))
            throw std::invalid_argument("invalid portal range order");
    std::pmr::vector<std::byte> bytes(header+entry*ranges.size(),&memory);
    for(std::size_t n=0;n<magic.size();++n)bytes[n]=std::byte(magic[n]);
    put(bytes,8,ranges.size());std::copy(lookup.begin(),lookup.end(),bytes.begin()+16);bytes[48]=std::byte(kind);
    for(std::size_t n=0;n<ranges.size();++n){
        const auto at=header+entry*n;
        std::copy(ranges[n].connection.begin(),ranges[n].connection.end(),bytes.begin()+at);
        put(bytes,at+32,ranges[n].begin);put(bytes,at+40,ranges[n].end);
    }
    auto owned_path=path;
    auto block=ExperienceBlock::create_impl(path,identity,ExperienceBlock::header_bytes+
        ExperienceBlock::record_overhead+session.size()+source.size()+media.size()+bytes.size(),storage,true);
    PortalPage page(std::move(block),kind,lookup,ranges.size(),std::move(owned_path),storage);
    page.location_=page.block_.append({0,0,session,source,media,bytes});return page;
}
std::pmr::vector<PortalPage::Range> PortalPage::load(MemoryBudget& memory) const {
    using namespace architecture::kernel;
    auto stored=block_.read(location_,location_.bytes,memory);
    const auto view=stored.view();const auto bytes=view.content;
    if(view.session!=session||view.source!=source||view.media_type!=media||view.sequence||view.observed_at_ns||
        view.sender!=ExperienceSender::unspecified||bytes.size()!=header+entry*count_||get(bytes,8)!=count_||
        !std::equal(magic.begin(),magic.end(),bytes.begin(),[](char a,std::byte b){return std::byte(a)==b;})||
        !std::equal(lookup_.begin(),lookup_.end(),bytes.begin()+16)||bytes[48]!=std::byte(kind_))
        throw std::runtime_error("portal page binding mismatch");
    for(unsigned n=49;n<header;++n)if(bytes[n]!=std::byte{})throw std::runtime_error("portal page flags");
    std::pmr::vector<Range> ranges(&memory);ranges.reserve(count_);
    for(std::size_t n=0;n<count_;++n){
        const auto at=header+entry*n;Range value;
        std::copy_n(bytes.begin()+at,32,value.connection.begin());value.begin=get(bytes,at+32);value.end=get(bytes,at+40);
        if(!portal_range_valid(value)||(!ranges.empty()&&!portal_range_follows(ranges.back(),value)))
            throw std::runtime_error("invalid portal page range");
        ranges.push_back(value);
    }
    return ranges;
}
} // namespace swegca::vrs
