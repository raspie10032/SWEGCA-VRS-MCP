#include "vrs/runtime.hpp"

namespace swegca::vrs {
using namespace architecture;
using namespace architecture::kernel;
Runtime Runtime::create(const std::filesystem::path& root,const RuntimeConfig& config,MemoryBudget& memory) {
    return Runtime(root,config,memory,true);
}
Runtime Runtime::open(const std::filesystem::path& root,const RuntimeConfig& config,MemoryBudget& memory) {
    return Runtime(root,config,memory,false);
}
Runtime::Runtime(const std::filesystem::path& root,const RuntimeConfig& config,MemoryBudget& memory,bool create)
    :root_(root),config_(config),memory_(memory),sources_(root,memory,config.read_limit),
    main_(create ? PersistentMainGraph::create(root/"graph",config.main_identity,memory,config.initial_strength,config.policy,config.main_block_capacity,config.merge_workers)
                 : PersistentMainGraph::open(root/"graph",config.main_identity,memory,config.initial_strength,config.policy,sources_,config.merge_workers)) {
    sources_.release_caches();
}
Runtime::Active::Active(const std::filesystem::path& root,const DigestBytes& identity,std::string_view name,
    const RuntimeConfig& config,MemoryBudget& memory,const PersistentMainGraph& main,bool resume)
    :store(resume ? SessionStore::open(root,identity,memory)
                  : SessionStore::create(root,identity,name,config.session_block_capacity,memory)),
    runtime(store,memory,config.read_limit),router(runtime,memory) {
    router.mount_main(main); indexed_main=main.head();
}
void Runtime::start_session(const DigestBytes& identity,std::string_view name) {
    if(active_)throw std::logic_error("a session already owns the input route");
    active_.emplace(root_,identity,name,config_,memory_,main_,false);
}
void Runtime::resume_session(const DigestBytes& identity) {
    if(active_)throw std::logic_error("a session already owns the input route");
    active_.emplace(root_,identity,std::string_view{},config_,memory_,main_,true);
}
Runtime::Active& Runtime::require_session() {
    if(!active_)throw std::logic_error("no session owns the input route");
    return *active_;
}
const Runtime::Active& Runtime::require_session() const {
    if(!active_)throw std::logic_error("no session owns the input route");
    return *active_;
}
void Runtime::require_active() const {
    const auto& runtime=require_session().runtime;
    SessionPhase next;
    if(!runtime.usable()||!next_session_phase(runtime.phase(),SessionOperation::append,next))
        throw std::logic_error("session no longer accepts input");
}
const SessionRuntime& Runtime::session() const { return require_session().runtime; }
void Runtime::end_session() {
    auto& runtime=require_session().runtime;
    if(runtime.phase()==SessionPhase::active)runtime.end();
    if(runtime.phase()==SessionPhase::ended)runtime.publish_originals();
    if(!main_session_readable(runtime.phase(),runtime.usable()))
        throw std::logic_error("session closure requires recovery before handoff");
    // Publication is the durable queue entry. Release the temporary owner so
    // MainSources can acquire the ended store; graph consolidation is deferred.
    active_.reset();
}
ReceivedInput Runtime::receive(const OriginalExperienceView& original,std::uint64_t seed,std::uint64_t step) {
    auto recalled=input(original.media_type,original.content);
    auto recorded=retain(original,seed,step);
    return {std::move(recalled),std::move(recorded)};
}
InputRecall Runtime::input(std::string_view media,std::span<const std::byte> content) const {
    // No closure, pending merge or storage work is placed before Deja vu.
    return require_session().router.input(media,content);
}
ReplayedInput Runtime::replay(const InputRecall& recalled,std::size_t candidate) const {
    return require_session().router.replay(recalled,candidate);
}
ReEvidenceResult Runtime::re_evidence(const ReplayedInput& replayed,std::uint64_t seed,std::uint64_t step) const {
    return require_session().router.re_evidence(replayed,seed,step);
}
void Runtime::define_connection(const DigestBytes& identity) {
    require_active();active_->runtime.define_connection(identity,config_.initial_strength,config_.policy);
}
RecordedRefinement Runtime::observe(const DigestBytes& identity,const OriginalExperienceView& original,
    const EvidenceObservation& observation,std::uint64_t seed,std::uint64_t step) {
    require_active();return active_->runtime.observe(identity,original,observation,seed,step);
}
RecordedRefinement Runtime::retain(const OriginalExperienceView& original,std::uint64_t seed,std::uint64_t step) {
    require_active();return active_->runtime.retain_input(original,config_.initial_strength,config_.policy,seed,step);
}
std::size_t Runtime::work(std::uint64_t seed,std::uint64_t step) {
    const auto count=sources_.merge_published(main_,seed,step);
    if(active_&&active_->indexed_main!=main_.head()) {
        active_->router.mount_main(main_);
        active_->indexed_main=main_.head();
    }
    return count;
}
} // namespace swegca::vrs
