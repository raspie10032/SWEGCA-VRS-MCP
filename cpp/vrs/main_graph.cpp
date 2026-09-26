#include "vrs/main_graph.hpp"
#include "swegca_architecture/sha256.hpp"
#include <thread>
#include <exception>

namespace swegca::vrs {
using namespace architecture;
using namespace architecture::kernel;

MainGraph::Entry::Entry(const DigestBytes& identity, double strength, const EvidenceRules& rules, MemoryBudget& memory)
    : connection(identity, strength, rules, memory), origins(&memory) {}
MainGraph::MainGraph(MemoryBudget& memory, double initial_strength, const EvidencePolicy& policy, std::uint32_t workers)
    : memory_(memory), initial_strength_(initial_strength), workers_(workers), rules_(make_evidence_rules(policy)),
      policy_digest_(evidence_policy_digest(policy).bytes()), connections_(&memory), merged_(&memory) {
    if (!workers_) throw std::invalid_argument("Main merge worker count must be positive");
    if (!finite_count(initial_strength)) throw std::invalid_argument("invalid Main initial strength");
}
bool MainGraph::merge(const SessionRuntime& source, std::uint64_t seed, std::uint64_t step) {
    return merge_impl(source, seed, step, nullptr, nullptr);
}
bool MainGraph::merge_impl(const SessionRuntime& source, std::uint64_t seed, std::uint64_t step,
    MergeSink sink, void* context) {
    if (!main_session_readable(source.phase(), source.usable()))
        throw std::logic_error("Main merge requires an ended published session");
    const auto identity = source.store_.identity();
    const auto existing = merged_.find(identity);
    if (existing != merged_.end()) {
        if (existing->second != source.catalog_.root()) throw std::logic_error("merged session root changed");
        return false;
    }
    if (source.catalog_.heads().empty()) return false;
    if (!catalog_root_ready(true, generation_)) throw std::overflow_error("Main generation exhausted");

    std::pmr::map<DigestBytes, Entry> pending(&memory_);
    std::pmr::map<DigestBytes, ExperienceLocation> marker(&memory_);
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
        const auto strength = previous == connections_.end() ? initial_strength_ : previous->second.connection.strength();
        auto& candidate = pending.try_emplace(connection_id, connection_id, strength, rules_, memory_).first->second;
        tasks.push_back({&candidate, previous == connections_.end() ? nullptr : &previous->second, incoming});
    }
    const auto prepare = [&](const Task& task) {
        auto& candidate = *task.candidate;
        if (task.previous) {
            for (const auto& value : task.previous->connection.experiences()) candidate.connection.append(value);
            candidate.origins.assign(task.previous->origins.begin(), task.previous->origins.end());
        }
        const auto added = task.incoming->state().experiences();
        candidate.origins.reserve(candidate.origins.size() + added.size());
        for (std::size_t index = 0; index < added.size(); ++index) {
            candidate.connection.append(added[index]);
            candidate.origins.push_back({&source.store_, source.read_limit_});
        }
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
    if (sink) {
        Sha256 hash; hash.update("SWEGCA Main merge result v1"); hash.update(policy_digest_);
        for (const auto& [id, candidate] : pending) {
            hash.update(id); hash.update(refinement_digest(*candidate.report));
            DigestBytes originals{};
            for (const auto& value : candidate.connection.experiences())
                originals = extend_experience_digest(originals, value.original());
            hash.update(originals);
        }
        sink(context, identity, source.catalog_.root(), hash.finish(), generation_ + 1, seed, step);
    }
    // No allocations or fallible persistence follow this point. Transfer the
    // prepared nodes under Main's serialized ownership, then publish generation.
    for (const auto& [connection_id, entry] : pending) {
        (void)entry;
        connections_.erase(connection_id);
    }
    connections_.merge(pending);
    merged_.merge(marker);
    ++generation_;
    return true;
}
const Connection* MainGraph::find(const DigestBytes& identity) const noexcept {
    const auto found = connections_.find(identity);
    return found == connections_.end() ? nullptr : &found->second.connection;
}
const ConnectionRefinement* MainGraph::refinement(const DigestBytes& identity) const noexcept {
    const auto found = connections_.find(identity);
    return found == connections_.end() ? nullptr : &*found->second.report;
}
StoredExperience MainGraph::replay(const DigestBytes& identity, std::size_t index) const {
    const auto found = connections_.find(identity);
    if (found == connections_.end() || index >= found->second.origins.size())
        throw std::out_of_range("Main original selection");
    const auto& origin = found->second.origins[index];
    if (!main_session_readable(origin.store->phase(), origin.store->usable()))
        throw std::logic_error("Main original source unavailable");
    auto result = origin.store->read(found->second.connection.experiences()[index].original(), origin.read_limit);
    if (result.location() != found->second.connection.experiences()[index].original())
        throw std::runtime_error("Main original provenance mismatch");
    return result;
}

}  // namespace swegca::vrs
