#pragma once
#include "vrs/experience_block.hpp"
#include "swegca_architecture/portal_range_kernel.hpp"
namespace swegca::vrs {
// Derived address ranges, not originals or new observations. Main must validate
// referenced connections before constructing a Recall receipt from these ranges.
class PortalPage final {
public:
    enum class Kind : std::uint8_t { cue=1,context=2 };
    static constexpr std::size_t capacity=256;
    using Range=architecture::kernel::PortalRange;
    [[nodiscard]] static PortalPage create(const std::filesystem::path&,const architecture::DigestBytes& identity,
        Kind,const architecture::DigestBytes& lookup,std::span<const Range>,MemoryBudget&,StorageBudget* = nullptr);
    PortalPage(const PortalPage&)=delete;
    PortalPage& operator=(const PortalPage&)=delete;
    PortalPage(PortalPage&&) noexcept;
    PortalPage& operator=(PortalPage&&)=delete;
    ~PortalPage();
    [[nodiscard]] std::pmr::vector<Range> load(MemoryBudget&) const;
    [[nodiscard]] std::size_t size() const noexcept{return count_;}
    [[nodiscard]] Kind kind() const noexcept{return kind_;}
    [[nodiscard]] const architecture::DigestBytes& lookup() const noexcept{return lookup_;}
private:
    friend class Runtime;
    static void reclaim_orphans(const std::filesystem::path&,MemoryBudget&,StorageBudget&);
    [[nodiscard]] std::pmr::vector<Range> decode(const StoredExperience&,MemoryBudget&) const;
    PortalPage(ExperienceBlock block,Kind kind,architecture::DigestBytes lookup,std::size_t count,
        std::filesystem::path path,StorageBudget* storage)
        :block_(std::move(block)),kind_(kind),lookup_(lookup),count_(count),path_(std::move(path)),
         charge_(storage?storage->used_:nullptr){}
    void discard() noexcept;
    ExperienceBlock block_;
    ExperienceLocation location_{};
    Kind kind_;
    architecture::DigestBytes lookup_;
    std::size_t count_;
    std::filesystem::path path_;
    std::shared_ptr<std::atomic<std::uint64_t>> charge_;
};
} // namespace swegca::vrs
