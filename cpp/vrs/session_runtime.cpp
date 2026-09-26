#include "vrs/session_runtime.hpp"

#include <algorithm>
#include <new>
#include <stdexcept>

namespace swegca::vrs {
using namespace architecture;
using namespace architecture::kernel;

SessionRuntime::Slot::Slot(SessionStore& store, MemoryBudget& budget, std::uint64_t limit,
    const ExperienceLocation& head) : memory(budget), connection(nullptr) {
    void* storage = memory.allocate(sizeof(PersistentConnection), alignof(PersistentConnection));
    try { connection = new (storage) PersistentConnection(PersistentConnection::recover(store, head, memory, limit)); }
    catch (...) { memory.deallocate(storage, sizeof(PersistentConnection), alignof(PersistentConnection)); throw; }
}
SessionRuntime::Slot::Slot(SessionStore& store, MemoryBudget& budget, std::uint64_t limit,
    const DigestBytes& identity, double strength, const EvidencePolicy& policy) : memory(budget), connection(nullptr) {
    void* storage = memory.allocate(sizeof(PersistentConnection), alignof(PersistentConnection));
    try { connection = new (storage) PersistentConnection(PersistentConnection::create(store, identity, strength, policy, memory, limit)); }
    catch (...) { memory.deallocate(storage, sizeof(PersistentConnection), alignof(PersistentConnection)); throw; }
}
SessionRuntime::Slot::~Slot() {
    connection->~PersistentConnection();
    memory.deallocate(connection, sizeof(PersistentConnection), alignof(PersistentConnection));
}
SessionRuntime::SessionRuntime(SessionStore& store, MemoryBudget& memory, std::uint64_t limit)
    : store_(store), memory_(memory), read_limit_(limit), catalog_(store, memory, limit), connections_(&memory) {
    for (const auto& [identity, head] : catalog_.heads())
        connections_.try_emplace(identity, store_, memory_, read_limit_, head.record);
}
void SessionRuntime::require_usable() const {
    if (!usable()) throw std::logic_error("session runtime unavailable; reopen required");
}
void SessionRuntime::define_connection(const DigestBytes& identity, double strength, const EvidencePolicy& policy) {
    require_usable();
    if (connections_.contains(identity)) throw std::invalid_argument("connection already defined");
    connections_.try_emplace(identity, store_, memory_, read_limit_, identity, strength, policy);
}
ExperienceLocation SessionRuntime::record(const OriginalExperienceView& original) {
    require_usable();
    try {
        const auto saved = store_.append(original);
        if (catalog_.generation() == 0) catalog_.publish({});
        return saved;
    } catch (...) { usable_ = false; throw; }
}
RecordedRefinement SessionRuntime::observe(const DigestBytes& identity, const OriginalExperienceView& original,
    const EvidenceObservation& observation, std::uint64_t seed, std::uint64_t step) {
    require_usable();
    const auto found = connections_.find(identity);
    if (found == connections_.end()) throw std::invalid_argument("connection not defined");
    if (observation.hypothesis != identity) throw std::invalid_argument("observation connection mismatch");
    auto& connection = *found->second.connection;
    const auto* prior = catalog_.find(identity);
    const auto expected = prior ? prior->record : ExperienceLocation{};
    try {
        const auto saved = store_.append_evidence(make_evidence_rules(connection.policy()), original, observation);
        connection.append(saved.original());
        auto report = connection.refine(seed, step);
        const HeadUpdate update{&connection, expected};
        catalog_.publish(std::span(&update, 1));
        return {saved.original(), std::move(report)};
    } catch (...) { usable_ = false; throw; }
}
const PersistentConnection* SessionRuntime::find(const DigestBytes& identity) const {
    require_usable();
    if (!catalog_.find(identity)) return nullptr;
    const auto found = connections_.find(identity);
    if (found == connections_.end()) throw std::logic_error("published connection missing from runtime");
    return found->second.connection;
}
StoredExperience SessionRuntime::replay(const DigestBytes& identity, std::size_t index) const {
    const auto* connection = find(identity);
    if (!connection || index >= connection->state().experiences().size())
        throw std::out_of_range("selected original not in recalled connection");
    return store_.read(connection->state().experiences()[index].original(), read_limit_);
}
void SessionRuntime::end() { require_usable(); store_.end(); }
void SessionRuntime::publish_originals() { require_usable(); store_.publish_originals(); }

RecallMatch RecallCandidates::at(std::size_t index) const {
    if (index >= size()) throw std::out_of_range("recall candidate");
    return temporary_.session ? temporary_ : main_[index];
}
ExperienceRouter::ExperienceRouter(SessionRuntime& temporary, MemoryBudget& memory)
    : temporary_(temporary), mounted_(&memory), main_(&memory) {}
void ExperienceRouter::mount_main(const SessionRuntime& session) {
    if (!main_session_readable(session.phase(), session.usable()))
        throw std::logic_error("Main requires a published ended session");
    if (std::find(mounted_.begin(), mounted_.end(), &session) != mounted_.end()) return;
    mounted_.reserve(mounted_.size() + 1);
    // Reserve every destination before exposing any candidate. Allocation
    // failure can leave empty index nodes, never a partially mounted session.
    for (const auto& [identity, head] : session.catalog_.heads()) {
        (void)head;
        auto [where, inserted] = main_.try_emplace(identity);
        (void)inserted;
        where->second.reserve(where->second.size() + 1);
    }
    for (const auto& [identity, head] : session.catalog_.heads()) {
        (void)head;
        main_.find(identity)->second.push_back({&session, session.connections_.find(identity)->second.connection});
    }
    mounted_.push_back(&session);
}
RecallCandidates ExperienceRouter::recall(const DigestBytes& identity) const {
    RecallCandidates result;
    const auto* local = temporary_.usable() ? temporary_.find(identity) : nullptr;
    switch (recall_scope(temporary_.usable(), local != nullptr)) {
    case RecallScope::unavailable: throw std::logic_error("temporary experience unavailable");
    case RecallScope::temporary: result.temporary_ = {&temporary_, local}; return result;
    case RecallScope::main: break;
    }
    const auto found = main_.find(identity);
    if (found != main_.end()) result.main_ = found->second;
    return result;
}
StoredExperience ExperienceRouter::replay(const RecallCandidates& candidates, std::size_t candidate,
    std::size_t original_index) const {
    const auto selected = candidates.at(candidate);
    return selected.session->replay(selected.connection->state().identity(), original_index);
}

}  // namespace swegca::vrs
