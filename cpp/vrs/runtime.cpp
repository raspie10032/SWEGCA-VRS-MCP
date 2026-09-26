#include "vrs/runtime.hpp"
#include "vrs/storage_inventory.hpp"

#ifdef SWEGCA_BACKGROUND_WORK_PROBE
extern "C" void swegca_background_work_probe(bool running) noexcept;
#endif

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
    :root_(root),config_(config),memory_(memory),storage_root_(root),storage_(config.storage_bytes,stored_bytes(root,memory),config.io_bytes_per_second),sources_(root,memory,config.read_limit,&storage_,config.merge_workers),
    main_(create ? PersistentMainGraph::create(root/"graph",config.main_identity,memory,config.initial_strength,config.policy,config.main_block_capacity,config.merge_workers,&storage_)
                 : PersistentMainGraph::open(root/"graph",config.main_identity,memory,config.initial_strength,config.policy,sources_,config.merge_workers,&storage_)) {
    sources_.release_caches();
}
Runtime::Active::Active(SessionRuntime& session,const DigestBytes& id,MemoryBudget& memory,const PersistentMainGraph& main)
    :identity(id),runtime(session),router(runtime,memory) {
    router.mount_main(main); indexed_main=main.head();
}
Runtime::~Runtime() { discard_work(); }
void Runtime::discard_work() noexcept {
    if(!work_)return;
    const auto source=work_->source;
    const auto identity=source?work_->ids[work_->index]:DigestBytes{};
    // Joining/dropping prepared work is not session end or Main publication.
    work_.reset();
    if(source)sources_.release_preparation(identity);
}
void Runtime::start_session(const DigestBytes& identity,std::string_view name) {
    if(active_)throw std::logic_error("a session already owns the input route");
    auto& session=sources_.acquire_session(identity,name,config_.session_block_capacity,false);
    try{active_.emplace(session,identity,memory_,main_);}
    catch(...){sources_.release_session(identity,false);throw;}
}
void Runtime::resume_session(const DigestBytes& identity) {
    if(active_)throw std::logic_error("a session already owns the input route");
    auto& session=sources_.acquire_session(identity,{},config_.session_block_capacity,true);
    try{active_.emplace(session,identity,memory_,main_);}
    catch(...){sources_.release_session(identity,false);throw;}
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
    // Publication is the durable queue entry. Invalidate the input route, then
    // hand the already verified cache back to its stable Main-owned store.
    // Consolidation remains deferred until work(); no history replay is needed.
    const auto identity=active_->identity;
    active_.reset();
    sources_.release_session(identity,true);
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
    if(work_)throw std::logic_error("background Main preparation already scheduled");
    const auto count=sources_.merge_published(main_,seed,step);
    refresh_main();
    return count;
}
void Runtime::refresh_main() {
    if(active_&&active_->indexed_main!=main_.head()) {
        active_->router.mount_main(main_);
        active_->indexed_main=main_.head();
    }
}
bool Runtime::schedule_work(std::uint64_t seed,std::uint64_t step) {
    if(work_)throw std::logic_error("background Main preparation already scheduled");
    (void)main_.graph();refresh_main();
    work_.emplace(sources_.published(),seed,step);
    try {
        if(launch_next())return true;
        discard_work();return false;
    } catch(...) { discard_work();throw; }
}
bool Runtime::launch_next() {
    auto& work=*work_;
    while(work.index<work.ids.size()&&main_.graph().has_source(work.ids[work.index]))++work.index;
    if(work.index==work.ids.size())return false;
    work.source=&sources_.acquire_preparation(work.ids[work.index]);
    work.done.store(false,std::memory_order_relaxed);
    work.thread=std::jthread([this] {
        auto& job=*work_;
#ifdef SWEGCA_BACKGROUND_WORK_PROBE
        swegca_background_work_probe(true);
#endif
        try { job.prepared.emplace(main_.prepare_merge(job.source->runtime(),job.seed,job.step)); }
        catch(...) { job.failure=std::current_exception(); }
#ifdef SWEGCA_BACKGROUND_WORK_PROBE
        swegca_background_work_probe(false);
#endif
        job.done.store(true,std::memory_order_release);
    });
    return true;
}
std::optional<std::size_t> Runtime::poll_work() {
    if(!work_)return std::size_t{0};
    if(!work_->done.load(std::memory_order_acquire))return std::nullopt;
    work_->thread.join();
    try {
        if(work_->failure)std::rethrow_exception(work_->failure);
        if(main_.commit_merge(std::move(*work_->prepared)))++work_->merged;
        work_->prepared.reset();
        sources_.release_preparation(work_->ids[work_->index]);
        work_->source=nullptr;++work_->index;
        refresh_main();
        if(launch_next())return std::nullopt;
        const auto count=work_->merged;discard_work();return count;
    } catch(...) { discard_work();throw; }
}
} // namespace swegca::vrs
