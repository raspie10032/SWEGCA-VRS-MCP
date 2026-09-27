#include "vrs/runtime.hpp"
#include "vrs/portal_page.hpp"
#include "vrs/storage_inventory.hpp"
#include "swegca_architecture/sha256.hpp"
#include "swegca_architecture/input_cue.hpp"

#ifdef SWEGCA_BACKGROUND_WORK_PROBE
extern "C" void swegca_background_work_probe(bool running) noexcept;
#endif

namespace swegca::vrs {
using namespace architecture;
using namespace architecture::kernel;
namespace {
// Captured identity only: worker never reads Main head or the owner's counter.
std::pair<std::filesystem::path,DigestBytes> metadata_destination(const std::filesystem::path& root,const DigestBytes& seed) {
    const auto directory=root/"metadata-pages";
    const auto status=std::filesystem::symlink_status(directory);
    if(status.type()==std::filesystem::file_type::not_found)std::filesystem::create_directory(directory);
    else if(!std::filesystem::is_directory(status))throw std::runtime_error("invalid metadata page directory");
    auto identity=seed;
    for(std::uint64_t collision=0;;){
        constexpr char digits[]="0123456789abcdef";std::string name;name.reserve(70);
        for(const auto byte:identity){const auto n=std::to_integer<unsigned>(byte);name+=digits[n>>4];name+=digits[n&15];}
        name+=".block";const auto path=directory/name;
        if(std::filesystem::symlink_status(path).type()==std::filesystem::file_type::not_found)return {path,identity};
        if(collision==UINT64_MAX)throw std::overflow_error("metadata page collisions exhausted");
        Sha256 hash;hash.update("SWEGCA metadata page collision v1");hash.update(seed);
        hash.update(std::to_string(++collision));identity=hash.finish();
    }
}
// Called only after StorageRoot has acquired exclusive ownership. A partially
// initialized or nonempty unrecognized root is never treated as a fresh store.
bool create_missing_main(const std::filesystem::path& root){
    if(std::filesystem::is_empty(root))return true;
    const auto status=std::filesystem::symlink_status(root/"graph");
    if(status.type()!=std::filesystem::file_type::directory)
        throw std::runtime_error("nonempty VRS root lacks a valid Main directory");
    return false;
}
}
Runtime Runtime::ensure(const std::filesystem::path& root,const RuntimeConfig& config,MemoryBudget& memory) {
    return Runtime(root,config,memory,false,true);
}
Runtime Runtime::create(const std::filesystem::path& root,const RuntimeConfig& config,MemoryBudget& memory) {
    return Runtime(root,config,memory,true);
}
Runtime Runtime::open(const std::filesystem::path& root,const RuntimeConfig& config,MemoryBudget& memory) {
    return Runtime(root,config,memory,false);
}
Runtime::Runtime(const std::filesystem::path& root,const RuntimeConfig& config,MemoryBudget& memory,bool create,bool discover)
    :root_(root),config_(config),memory_(memory),storage_root_(root),storage_(config.storage_bytes,stored_bytes(root,memory),config.io_bytes_per_second,StorageBudget::Recovery{}),sources_(root,memory,config.read_limit,&storage_,config.merge_workers),
    main_((discover?create_missing_main(root):create) ? PersistentMainGraph::create(root/"graph",config.main_identity,memory,config.initial_strength,config.policy,config.main_block_capacity,config.merge_workers,&storage_)
                 : PersistentMainGraph::open(root/"graph",config.main_identity,memory,config.initial_strength,config.policy,sources_,config.merge_workers,&storage_)),sessions_(&memory) {
    sources_.release_caches();
    ExperiencePage::reclaim_orphans(root_/"metadata-pages",make_evidence_rules(config.policy),memory_,storage_);
    PortalPage::reclaim_orphans(root_/"portal-pages",memory_,storage_);
    if(storage_.used()>storage_.limit())throw StorageLimit();
}
Runtime::Active::Active(SessionRuntime& session,const DigestBytes& id,MemoryBudget& memory,const PersistentMainGraph& main)
    :identity(id),runtime(session),router(runtime,memory) {
    router.mount_main(main); indexed_main=main.head();
    if(const auto position=runtime.read_replay_position())router.restore_position(*position);
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
void Runtime::attach(const DigestBytes& identity,std::string_view name,bool resume) {
    if(sessions_.contains(identity))throw std::logic_error("session route already attached");
    auto& session=sources_.acquire_session(identity,name,config_.session_block_capacity,resume);
    try{sessions_.try_emplace(identity,session,identity,memory_,main_);}
    catch(...){sources_.release_session(identity,false);throw;}
}
void Runtime::attach_session(const DigestBytes& identity,std::string_view name) { attach(identity,name,false); }
void Runtime::attach_resumed_session(const DigestBytes& identity) { attach(identity,{},true); }
void Runtime::attach_available_session(const DigestBytes& identity,std::string_view name) {
    if(sessions_.contains(identity))throw std::logic_error("session route already attached");
    auto& session=sources_.acquire_session(identity,name,config_.session_block_capacity,sources_.contains_session(identity));
    try{
        SessionPhase next;
        if(!session.usable()||!next_session_phase(session.phase(),SessionOperation::append,next))
            throw std::invalid_argument("stored session no longer accepts lifecycle events");
        sessions_.try_emplace(identity,session,identity,memory_,main_);
    }catch(...){sources_.release_session(identity,false);throw;}
}
void Runtime::select_session(const DigestBytes& identity) {
    const auto found=sessions_.find(identity);
    if(found==sessions_.end())throw std::invalid_argument("session route is not attached");
    active_=&found->second;
}
void Runtime::start_session(const DigestBytes& identity,std::string_view name) {
    if(active_)throw std::logic_error("a session already owns the input route");
    attach_session(identity,name);select_session(identity);
}
void Runtime::resume_session(const DigestBytes& identity) {
    if(active_)throw std::logic_error("a session already owns the input route");
    attach_resumed_session(identity);select_session(identity);
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
const SessionRuntime& Runtime::attached_session(const DigestBytes& identity) const {
    const auto found=sessions_.find(identity);
    if(found==sessions_.end())throw std::invalid_argument("session not attached");
    return found->second.runtime;
}
void Runtime::end_session() {
    end_session(require_session().identity);
}
void Runtime::end_session(const DigestBytes& requested) {
    const auto found=sessions_.find(requested);
    if(found==sessions_.end())throw std::invalid_argument("session not attached");
    auto& runtime=found->second.runtime;
    // Reserve a queue slot before the irreversible end/publication. The worker
    // never reads ids; only the serialized owner consumes this queue in poll.
    if(work_ && work_->ids.size()==work_->ids.capacity()){
        auto& ids=work_->ids;
        if(ids.size()==ids.max_size())throw std::length_error("Main work queue exhausted");
        const auto capacity=ids.capacity();
        ids.reserve(capacity ? (capacity>ids.max_size()/2 ? ids.max_size() : capacity*2) : 1);
    }
    if(runtime.phase()==SessionPhase::active)runtime.end();
    if(runtime.phase()==SessionPhase::ended)runtime.publish_originals();
    if(!main_session_readable(runtime.phase(),runtime.usable()))
        throw std::logic_error("session closure requires recovery before handoff");
    // Publication is the durable queue entry. Invalidate this session route, then
    // hand the already verified cache back to its stable Main-owned store.
    // Consolidation remains deferred until work/poll; end never merges.
    // A running batch picks up this newly published source without a rescan.
    const auto identity=found->first;
    if(active_==&found->second)active_=nullptr;
    sessions_.erase(found);
    sources_.release_session(identity,true);
    if(work_)work_->ids.push_back(identity); // reserved, fixed-size, no allocation
}
ReceivedInput Runtime::receive_envelope(std::string_view media,std::span<const std::byte> content,
    const OriginalExperienceView& envelope,std::uint64_t seed,std::uint64_t step) {
    auto recalled=input(media,content);
    require_active();
    auto recorded=active_->runtime.retain_input(envelope,config_.initial_strength,config_.policy,
        seed,step,recalled.cue());
    return {std::move(recalled),std::move(recorded)};
}
ReceivedInput Runtime::receive(const OriginalExperienceView& original,std::uint64_t seed,std::uint64_t step) {
    auto recalled=input(original.media_type,original.content);
    require_active();
    auto recorded=require_session().runtime.retain_input(original,config_.initial_strength,config_.policy,
        seed,step,recalled.cue());
    return {std::move(recalled),std::move(recorded)};
}
InputRecall Runtime::input(std::string_view media,std::span<const std::byte> content) const {
    // No closure, pending merge or storage work is placed before Deja vu.
    return require_session().router.input(media,content);
}
InputRecall Runtime::input_scope(const InputRecall& parent,std::string_view scope) const {
    return require_session().router.input_scope(parent,scope);
}
InputRecall Runtime::related(const ReplayedInput& parent,const DigestBytes* connection) const {
    return require_session().router.related(parent,connection);
}
RelatedConnectionPage Runtime::related_connections(const ReplayedInput& parent,std::size_t limit,const DigestBytes* after) const {
    return require_session().router.related_connections(parent,limit,after);
}
std::optional<InputCognition> Runtime::cognize(const InputRecall& recalled,
    std::uint64_t seed,std::uint64_t step) const {
    const auto candidate=select_replay(recalled);
    if(!candidate)return std::nullopt;
    return cognize_selected(recalled,*candidate,seed,step);
}
InputCognition Runtime::cognize_selected(const InputRecall& recalled,std::size_t candidate,
    std::uint64_t seed,std::uint64_t step) const {
    auto original=replay(recalled,candidate);
    auto compared=compare_replay(original,seed,step);
    std::optional<ReEvidenceResult> reverified;
    if(architecture::kernel::requires_re_evidence(compared.agreement()))
        reverified.emplace(re_evidence(original,compared,seed,step));
    return InputCognition{candidate,std::move(original),std::move(compared),std::move(reverified)};
}
InputCognition Runtime::restore_temporary_cognition(const ExperienceLocation& input,
    std::string_view scope,const DigestBytes& connection,const ExperienceLocation& remembered_head,std::size_t original_index,
    const ExperienceLocation& original,std::uint64_t seed,std::uint64_t step) const {
    require_active();
    auto replayed=require_session().router.restore_temporary_replay(input,scope,connection,
        remembered_head,original_index,original);
    auto compared=compare_replay(replayed,seed,step);
    std::optional<ReEvidenceResult> verified;
    if(requires_re_evidence(compared.agreement()))verified.emplace(re_evidence(replayed,compared,seed,step));
    require_session().runtime.save_replay_position(replayed.position());
    return {original_index,std::move(replayed),std::move(compared),std::move(verified)};
}
InputCognition Runtime::restore_main_cognition(const ExperienceLocation& input,
    std::string_view scope,const DigestBytes& connection,const ExperienceLocation& remembered_head,
    const ExperienceLocation& observation_head,std::size_t original_index,const ExperienceLocation& original,
    std::uint64_t seed,std::uint64_t step) const {
    require_active();
    auto replayed=require_session().router.restore_main_replay(input,scope,connection,remembered_head,
        observation_head,original_index,original);
    auto compared=compare_replay(replayed,seed,step);
    std::optional<ReEvidenceResult> verified;
    if(requires_re_evidence(compared.agreement()))verified.emplace(re_evidence(replayed,compared,seed,step));
    require_session().runtime.save_replay_position(replayed.position());
    return {original_index,std::move(replayed),std::move(compared),std::move(verified)};
}
InputCognition Runtime::restore_cognition(const ExperienceLocation& input,const ReplayRecovery& saved,
    std::uint64_t seed,std::uint64_t step) const {
    require_active();
    auto replayed=require_session().router.restore_replay(input,saved);
    auto compared=compare_replay(replayed,seed,step);
    std::optional<ReEvidenceResult> verified;
    if(requires_re_evidence(compared.agreement()))verified.emplace(re_evidence(replayed,compared,seed,step));
    require_session().runtime.save_replay_position(replayed.position());
    return {saved.original_index,std::move(replayed),std::move(compared),std::move(verified)};
}
std::optional<std::size_t> Runtime::select_replay(const InputRecall& recalled,const DigestBytes* connection) const {
    return require_session().router.select_replay(recalled,connection);
}
ReplayConnectionPage Runtime::select_replay_connections(const InputRecall& recalled,
    std::size_t limit,const DigestBytes* after) const {
    return require_session().router.select_replay_connections(recalled,limit,after);
}
ReplayedInput Runtime::replay(const InputRecall& recalled,std::size_t candidate) const {
    auto replayed=require_session().router.replay(recalled,candidate);
    require_session().runtime.save_replay_position(replayed.position());
    return replayed;
}
EvidencePayloadSlice Runtime::read_payload_slice(const InputRecall& recalled,std::size_t candidate,
    std::uint64_t offset,std::uint64_t count) const {
    return require_session().router.read_payload_slice(recalled,candidate,offset,count);
}
ReplayComparison Runtime::compare_replay(const ReplayedInput& replayed,std::uint64_t seed,std::uint64_t step) const {
    return require_session().router.compare_replay(replayed,seed,step);
}
ReEvidenceResult Runtime::re_evidence(const ReplayedInput& replayed,const ReplayComparison& compared,
    std::uint64_t seed,std::uint64_t step) const {
    return require_session().router.re_evidence(replayed,compared,seed,step);
}
void Runtime::save_cognition(const ExperienceLocation& input,std::span<const std::byte> metadata) {
    require_active();require_session().runtime.save_cognition(input,metadata);
}
DigestBytes Runtime::save_cognition_revision(const ExperienceLocation& input,std::span<const std::byte> metadata,std::optional<DigestBytes> channel) {
    require_active();return require_session().runtime.save_cognition_revision(input,metadata,channel);
}
StoredExperience Runtime::read_cognition_original(const DigestBytes& source,const ExperienceLocation& original) const {
    const auto found=sources_.sources_.find(source);
    if(found==sources_.sources_.end() || !found->second.store || !found->second.store->usable())
        throw std::invalid_argument("recorded cognition source unavailable");
    // Source stores already belong to this Main. No directory search or graph
    // reselection, and no reconstruction of a current Re-evidence authority.
    return found->second.store->read(original,config_.read_limit);
}
StoredExperience Runtime::read_scoped_cognition_original(const ExperienceLocation& input,std::string_view scope,const DigestBytes& connection,
    const DigestBytes& source,const ExperienceLocation& original) const {
    if(scope.empty())throw std::invalid_argument("recorded scope requires a name");
    const auto rules=make_evidence_rules(config_.policy);
    const auto parent=require_session().runtime.read_original(input);
    const auto parent_evidence=decode_evidence(rules,parent);
    const auto expected=input_observation_scope(parent_evidence.value().hypothesis,scope);
    if(connection!=expected)throw std::invalid_argument("recorded scope connection mismatch");
    auto selected=read_cognition_original(source,original);
    const auto evidence=decode_evidence(rules,selected);
    if(evidence.value().hypothesis!=expected||!evidence.has_input_key()||evidence.cue()!=expected)
        throw std::invalid_argument("recorded scope original mismatch");
    return selected;
}
std::optional<StoredExperience> Runtime::read_cognition_record(const DigestBytes& source,
    const ExperienceLocation& input,std::optional<DigestBytes> revision,bool latest,std::optional<DigestBytes> channel) const {
    if(latest&&revision)throw std::invalid_argument("choose latest or revision");
    const auto found=sources_.sources_.find(source);
    if(found==sources_.sources_.end()||!found->second.store||!found->second.store->usable())
        throw std::invalid_argument("recorded cognition source unavailable");
    // Read a Main-owned immutable record without acquiring an input route or
    // changing the currently selected session, including after publication.
    if(latest)return found->second.store->read_latest_cognition(input,channel);
    return revision?found->second.store->read_cognition_revision(input,*revision):
        found->second.store->read_cognition(input);
}
bool Runtime::maintain_memory() {
    if(page_work_){
        if(!page_work_->done.load(std::memory_order_acquire))return true;
        if(work_)return false; // no metadata release while Main preparation reads it
        page_work_->thread.join();
        try {
            if(page_work_->failure)std::rethrow_exception(page_work_->failure);
            (void)page_work_->prepared.commit();
        }catch(const std::bad_alloc&){page_work_.reset();return false;}
        catch(const StorageLimit&){page_work_.reset();return false;}
        catch(...){page_work_.reset();throw;}
        page_work_.reset();
    }
    const auto target=config_.memory_target_bytes?config_.memory_target_bytes:memory_.limit()-memory_.limit()/4;
    if(!metadata_pressure(memory_.used(),target,work_.has_value()))return false;
    const auto& graph=main_.graph();
    const auto key=graph.next_connection(page_cursor_);
    if(!key){page_cursor_={};page_index_=0;return false;}
    if(*key!=page_cursor_){page_cursor_=*key;page_index_=0;}
    const auto index=page_index_;
    const bool candidate=graph.page_candidate(*key,page_index_);
    if(page_index_==index){
        // Advance the fixed-width address without a second graph walk.
        page_index_=0;
        for(std::size_t n=page_cursor_.size();n;--n){
            auto value=std::to_integer<unsigned>(page_cursor_[n-1]);
            page_cursor_[n-1]=std::byte((value+1)&255);
            if(value!=255)return true;
        }
        return false;
    }
    if(candidate){
        try {
            auto prepared=graph.prepare_page(*key,index);
            if(prepared){
                const auto seed=page_identity(*key,index);
                page_work_.emplace(std::move(*prepared));
                page_work_->thread=std::jthread([this,seed]{
                    auto& job=*page_work_;
                    try {
                        const auto [path,identity]=metadata_destination(root_,seed);
                        job.prepared.write(path,identity,&storage_);
                    }
                    catch(...){job.failure=std::current_exception();}
                    job.done.store(true,std::memory_order_release);
                });
                return true;
            }
        }catch(const std::bad_alloc&){page_work_.reset();return false;}
        catch(const StorageLimit&){page_work_.reset();return false;}
        catch(...){page_work_.reset();throw;}
    }
    return metadata_pressure(memory_.used(),target,false);
}
bool Runtime::page_out_main(const DigestBytes& connection,std::size_t index) {
    if(work_)throw std::logic_error("cannot page out during Main preparation");
    const auto& graph=main_.graph();
    const auto* found=graph.find(connection);
    if(!found||index>=found->experiences().size())throw std::out_of_range("Main page-out original");
    const auto [path,identity]=metadata_destination(root_,page_identity(connection,index));
    return graph.page_out(connection,index,path,identity,&storage_);
}
DigestBytes Runtime::page_identity(const DigestBytes& connection,std::size_t index) {
    if(page_attempt_==UINT64_MAX)throw std::overflow_error("metadata page attempts exhausted");
    Sha256 hash;hash.update("SWEGCA derived metadata page v1");hash.update(config_.main_identity);
    hash.update(main_.head().digest);hash.update(connection);
    hash.update(":"+std::to_string(index)+":"+std::to_string(++page_attempt_));
    return hash.finish();
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
RecordedRefinement Runtime::observe_input(const ExperienceLocation& input,const OriginalExperienceView& original,
    const EvidenceObservation& observation,std::uint64_t seed,std::uint64_t step) {
    require_active();
    if(named_digest(observation.hypothesis)||named_digest(observation.context))
        throw std::invalid_argument("input observation cannot override hypothesis or context");
    const auto stored=active_->runtime.read_original(input);
    const auto rules=make_evidence_rules(config_.policy);
    const auto evidence=decode_evidence(rules,stored);
    auto bound=observation;
    bound.hypothesis=evidence.value().hypothesis;
    bound.context=input.digest;
    // The original's cue links the outcome back to that exact natural input;
    // recording different result text must not disconnect future Recall.
    return active_->runtime.observe(bound.hypothesis,original,bound,seed,step,evidence.cue());
}
RecordedRefinement Runtime::observe_input_scope(const ExperienceLocation& input,std::string_view scope,
    const OriginalExperienceView& original,const EvidenceObservation& observation,std::uint64_t seed,std::uint64_t step) {
    require_active();
    if(scope.empty()||named_digest(observation.hypothesis)||named_digest(observation.context))
        throw std::invalid_argument("scoped observation requires scope and no hypothesis or context override");
    const auto stored=active_->runtime.read_original(input);
    const auto rules=make_evidence_rules(config_.policy);
    const auto evidence=decode_evidence(rules,stored);
    auto bound=observation;
    bound.hypothesis=input_observation_scope(evidence.value().hypothesis,scope);
    bound.context=input.digest;
    if(!observation_values_valid(rules,bound.hypothesis,bound)||named_digest(bound.address)||
        bound.observed_at!=original.observed_at_ns||original.media_type.empty()||static_cast<unsigned>(original.sender)>2)
        throw std::invalid_argument("invalid scoped observation");
    active_->runtime.ensure_connection(bound.hypothesis,config_.initial_strength,config_.policy);
    return active_->runtime.observe(bound.hypothesis,original,bound,seed,step,bound.hypothesis);
}
std::size_t Runtime::work(std::uint64_t seed,std::uint64_t step) {
    if(work_)throw std::logic_error("background Main preparation already scheduled");
    const auto count=sources_.merge_published(main_,seed,step);
    refresh_main();
    return count;
}
void Runtime::refresh_main() {
    for(auto& [identity,session]:sessions_) {
        (void)identity;
        if(session.indexed_main==main_.head())continue;
        session.router.mount_main(main_);
        session.indexed_main=main_.head();
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
