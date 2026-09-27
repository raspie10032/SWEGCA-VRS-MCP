#include "vrs/main_graph.hpp"
#include "swegca_architecture/sha256.hpp"
#include <algorithm>
#include <thread>
#include <exception>
#include <utility>

namespace swegca::vrs {
using namespace architecture;
using namespace architecture::kernel;

MainGraph::Entry::Entry(const DigestBytes& identity, double strength, const EvidenceRules& rules, MemoryBudget& memory)
    : connection(identity, strength, rules, memory), origins(&memory) {}
MainGraph::MainGraph(MemoryBudget& memory, double initial_strength, const EvidencePolicy& policy, std::uint32_t workers, std::size_t region_capacity)
    : memory_(memory), initial_strength_(initial_strength), workers_(workers), rules_(make_evidence_rules(policy)),
      policy_digest_(evidence_policy_digest(policy).bytes()), connections_(memory,region_capacity),contexts_(&memory),cues_(&memory), merged_(&memory) {
    if (!workers_) throw std::invalid_argument("Main merge worker count must be positive");
    if (!finite_count(initial_strength)) throw std::invalid_argument("invalid Main initial strength");
}
bool MainGraph::merge(const SessionRuntime& source, std::uint64_t seed, std::uint64_t step) {
    return merge_impl(source, seed, step, nullptr, nullptr);
}
bool MainGraph::merge_impl(const SessionRuntime& source, std::uint64_t seed, std::uint64_t step,
    MergeSink sink, void* context) {
    auto prepared = prepare_merge(source, seed, step);
    return commit_impl(prepared, sink, context);
}
MainGraph::PreparedMerge::PreparedMerge(const MainGraph& owner, std::uint64_t seed, std::uint64_t step)
    : owner_(&owner), generation_(owner.generation_), seed_(seed), step_(step),
      pending_(&owner.memory_), marker_(&owner.memory_),contexts_(&owner.memory_),cues_(&owner.memory_) {}
MainGraph::PreparedMerge::PreparedMerge(PreparedMerge&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)), generation_(other.generation_), seed_(other.seed_), step_(other.step_),
      pending_(std::move(other.pending_)), marker_(std::move(other.marker_)),
      result_(other.result_), regions_(std::move(other.regions_)),contexts_(std::move(other.contexts_)),cues_(std::move(other.cues_)) {}
MainGraph::PreparedMerge MainGraph::prepare_merge(const SessionRuntime& source, std::uint64_t seed, std::uint64_t step) const {
    if (!main_session_readable(source.phase(), source.usable()))
        throw std::logic_error("Main merge requires an ended published session");
    const auto identity = source.store_.identity();
    PreparedMerge prepared(*this, seed, step);
    const auto existing = merged_.find(identity);
    if (existing != merged_.end()) {
        if (existing->second != source.catalog_.root()) throw std::logic_error("merged session root changed");
        return prepared;
    }
    if (source.catalog_.heads().empty()) return prepared;
    if (!catalog_root_ready(true, generation_)) throw std::overflow_error("Main generation exhausted");

    auto& pending = prepared.pending_;
    auto& marker = prepared.marker_;
    marker.emplace(identity, source.catalog_.root());
    struct Task { Entry* candidate; const Entry* previous; const PersistentConnection* incoming; };
    std::pmr::vector<Task> tasks(&memory_);
    tasks.reserve(source.catalog_.heads().size());
    for (const auto& [connection_id, head] : source.catalog_.heads()) {
        (void)head;
        const auto* incoming = source.find(connection_id);
        if (!incoming || evidence_policy_digest(incoming->policy()).bytes() != policy_digest_)
            throw std::invalid_argument("source connection policy differs from Main policy");
        const auto previous = connections_.find(connection_id);
        const auto strength = !previous ? initial_strength_ : previous->connection.strength();
        auto& candidate = pending.try_emplace(connection_id, connection_id, strength, rules_, memory_).first->second;
        tasks.push_back({&candidate, previous, incoming});
    }
    const auto prepare = [&](const Task& task) {
        auto& candidate = *task.candidate;
        if (task.previous) {
            candidate.connection.inherit_experiences(task.previous->connection);
            candidate.origins.assign(task.previous->origins.begin(), task.previous->origins.end());
        }
        const auto added = task.incoming->state().experiences();
        if (!added.empty()) candidate.origins.reserve(candidate.origins.size() + 1);
        if(!task.previous) {
            // A first Main connection can share sealed source segments under
            // the same VRS budget. Core admission and fresh shuffle still run;
            // source strength/revision are not installed as Main's judgment.
            candidate.connection.inherit_experiences(task.incoming->state());
        } else {
            for (std::size_t index = 0; index < added.size(); ++index)
                candidate.connection.append(added[index]);
        }
        if (!added.empty())
            candidate.origins.push_back({&source.store_, source.read_limit_, candidate.connection.experiences().size()});
        // Each connection keeps its exact serial shuffle and SWEGCA reduction.
        // Only independent connections run concurrently, into private candidates.
        candidate.report.emplace(candidate.connection.refine(seed, step));
        if (!candidate.report->result().strength().valid())
            throw std::runtime_error("SWEGCA rejected Main strength projection");
    };
    const auto count = std::min<std::size_t>(workers_, tasks.size());
    if (count < 2) {
        for (const auto& task : tasks) prepare(task);
    } else {
        std::pmr::vector<std::exception_ptr> failures(count, &memory_);
        // Declared after failures/tasks: failed thread creation also joins all
        // started workers before their candidate storage or captures disappear.
        std::pmr::vector<std::jthread> threads(&memory_);
        threads.reserve(count - 1);
        const auto run = [&](std::size_t worker) {
            try { for (std::size_t index = worker; index < tasks.size(); index += count) prepare(tasks[index]); }
            catch (...) { failures[worker] = std::current_exception(); }
        };
        for (std::size_t worker = 1; worker < count; ++worker) threads.emplace_back(run, worker);
        run(0);
        for (auto& thread : threads) thread.join();
        for (const auto& failure : failures) if (failure) std::rethrow_exception(failure);
    }
    {
        Sha256 hash; hash.update("SWEGCA Main merge result v1"); hash.update(policy_digest_);
        for (const auto& [id, candidate] : pending) {
            hash.update(id); hash.update(refinement_digest(*candidate.report));
            DigestBytes originals{};
            for (const auto& value : candidate.connection.experiences())
                originals = extend_experience_digest(originals, value.original());
            hash.update(originals);
        }
        prepared.result_ = hash.finish();
    }
    prepared.regions_.emplace(connections_.prepare(pending));
    for(const auto& [identity,candidate]:pending){
        const auto* previous=connections_.find(identity);
        const auto values=candidate.connection.experiences();
        for(std::size_t index=previous?previous->connection.experiences().size():0;index<values.size();++index){
            const auto add=[&](auto& lookup,const DigestBytes& cue){
                auto& ranges=lookup[cue];
                const PortalReference key{identity,index};const auto after=ranges.upper_bound(key);
                if(after!=ranges.begin()){
                    const auto prior=std::prev(after);
                    if(prior->first.first==identity&&prior->second==index){prior->second=index+1;return;}
                }
                ranges.emplace(key,index+1);
            };
            add(prepared.contexts_,values[index].value().context);
            add(prepared.cues_,values[index].cue());
        }
    }
    return prepared;
}
bool MainGraph::commit_merge(PreparedMerge&& prepared) {
    return commit_impl(prepared, nullptr, nullptr);
}
bool MainGraph::commit_impl(PreparedMerge& prepared, MergeSink sink, void* context) {
    if (prepared.owner_ != this) throw std::logic_error("Main merge batch is foreign, moved or consumed");
    if (prepared.generation_ != generation_) throw std::logic_error("Main changed after merge preparation");
    if (prepared.marker_.empty()) { prepared.owner_ = nullptr; return false; }
    const auto& [identity, root] = *prepared.marker_.begin();
    if (sink) sink(context, identity, root, prepared.result_, generation_ + 1, prepared.seed_, prepared.step_);
    prepared.owner_ = nullptr;
    auto& pending = prepared.pending_;
    // No allocations or fallible persistence follow this point. Transfer the
    // prepared nodes under Main's serialized ownership, then publish generation.
    connections_.commit(*prepared.regions_,pending);
    const auto publish_ranges=[](auto& index,auto& prepared_index){
        for(auto& [key,additions]:prepared_index){
            const auto found=index.find(key);if(found==index.end())continue;
            auto& existing=found->second;
            while(!additions.empty()){
                const auto added=additions.begin();const auto after=existing.upper_bound(added->first);
                if(after!=existing.begin()){
                    const auto prior=std::prev(after);
                    if(prior->first.first==added->first.first&&prior->second==added->first.second){
                        prior->second=added->second;additions.erase(added);continue;
                    }
                }
                existing.insert(additions.extract(added));
            }
        }
        index.merge(prepared_index);
    };
    publish_ranges(contexts_,prepared.contexts_);
    publish_ranges(cues_,prepared.cues_);
    merged_.merge(prepared.marker_);
    ++generation_;
    return true;
}
bool MainGraph::page_out(const DigestBytes& connection,std::size_t index,
    const std::filesystem::path& path,const DigestBytes& page,StorageBudget* storage) const {
    const auto* found=connections_.find(connection);
    if(!found)throw std::out_of_range("Main page-out connection");
    return found->connection.page_out(index,path,page,storage);
}
const Connection* MainGraph::find(const DigestBytes& identity) const noexcept {
    const auto found = connections_.find(identity);
    return !found ? nullptr : &found->connection;
}
const ConnectionRefinement* MainGraph::refinement(const DigestBytes& identity) const noexcept {
    const auto found = connections_.find(identity);
    return !found ? nullptr : &*found->report;
}
const MainGraph::Origin& MainGraph::original_source(const DigestBytes& identity, std::size_t index) const {
    const auto found = connections_.find(identity);
    if (!found || index >= found->connection.experiences().size())
        throw std::out_of_range("Main original selection");
    const auto& origins = found->origins;
    const auto selected = std::upper_bound(origins.begin(), origins.end(), index,
        [](std::size_t position, const Origin& range) { return position < range.end; });
    if (selected == origins.end()) throw std::logic_error("Main original source range missing");
    const auto& origin = *selected;
    if (!main_session_readable(origin.store->phase(), origin.store->usable()))
        throw std::logic_error("Main original source unavailable");
    return origin;
}
StoredExperience MainGraph::replay(const DigestBytes& identity,std::size_t index) const {
    const auto& origin=original_source(identity,index);
    const auto found=connections_.find(identity);
    auto result = origin.store->read(found->connection.experiences()[index].original(), origin.read_limit);
    if (result.location() != found->connection.experiences()[index].original())
        throw std::runtime_error("Main original provenance mismatch");
    return result;
}

EvidencePayloadSlice MainGraph::read_payload_slice(const DigestBytes& identity,std::size_t index,
    std::uint64_t offset,std::uint64_t count) const {
    const auto& origin=original_source(identity,index);
    const auto& original=connections_.find(identity)->connection.experiences()[index].original();
    return origin.store->read_payload_slice(rules_,original,origin.read_limit,offset,count);
}

}  // namespace swegca::vrs
