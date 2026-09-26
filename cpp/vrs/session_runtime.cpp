#include "vrs/session_runtime.hpp"
#include "swegca_architecture/input_cue.hpp"

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
    : store_(store), memory_(memory), read_limit_(limit), catalog_(store, memory, limit), connections_(&memory), cues_(&memory) {
    for (const auto& [identity, head] : catalog_.heads()) {
        auto [where, inserted] = connections_.try_emplace(identity, store_, memory_, read_limit_, head.record);
        (void)inserted;
        const auto experiences = where->second.connection->state().experiences();
        for (std::size_t i = 0; i < experiences.size(); ++i)
            cues_.try_emplace(experiences[i].cue()).first->second.push_back({identity, i});
    }
}
void SessionRuntime::require_usable() const {
    if (!usable()) throw std::logic_error("session runtime unavailable; reopen required");
}
void SessionRuntime::define_connection(const DigestBytes& identity, double strength, const EvidencePolicy& policy) {
    require_usable();
    if (connections_.contains(identity)) throw std::invalid_argument("connection already defined");
    connections_.try_emplace(identity, store_, memory_, read_limit_, identity, strength, policy);
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
    const auto cue = input_cue(original.media_type, original.content);
    auto& references = cues_.try_emplace(cue).first->second;
    references.reserve(references.size() + 1);
    const auto index = connection.state().experiences().size();
    try {
        const auto saved = store_.append_evidence(connection.rules(), original, observation);
        connection.append(saved.original());
        auto report = connection.refine(seed, step);
        const HeadUpdate update{&connection, expected};
        catalog_.publish(std::span(&update, 1));
        references.push_back({identity, index});
        return {saved.original(), std::move(report)};
    } catch (...) { usable_ = false; throw; }
}
RecordedRefinement SessionRuntime::retain_input(const OriginalExperienceView& original,
    double initial_strength, const EvidencePolicy& policy, std::uint64_t seed, std::uint64_t step) {
    require_usable();
    const auto identity = input_cue(original.media_type, original.content);
    if (!connections_.contains(identity)) define_connection(identity, initial_strength, policy);
    EvidenceObservation observation;
    observation.hypothesis = identity;
    Sha256 source; source.update("SWEGCA input source v1"); source.update(original.source);
    observation.source = observation.producer = source.finish();
    Sha256 context; context.update("SWEGCA input session v1"); context.update(original.session);
    observation.context = context.finish();
    observation.observed_at = original.observed_at_ns;
    return observe(identity, original, observation, seed, step);
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
    : temporary_(temporary), memory_(memory), mounted_(&memory), main_(&memory), main_cues_(&memory) {}
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
    for (const auto& [cue, references] : session.cues_) {
        auto& destination = main_cues_.try_emplace(cue).first->second;
        destination.reserve(destination.size() + references.size());
    }
    for (const auto& [identity, head] : session.catalog_.heads()) {
        (void)head;
        const auto* connection = session.connections_.find(identity)->second.connection;
        main_.find(identity)->second.push_back({&session, connection, connection->snapshot()});
    }
    for (const auto& [cue, references] : session.cues_)
        for (const auto& reference : references)
            main_cues_.find(cue)->second.push_back({&session, reference});
    mounted_.push_back(&session);
}
InputRecall ExperienceRouter::input(std::string_view media, std::span<const std::byte> content) const {
    // Deja vu: natural bytes reach the core cue primitive immediately. This
    // anonymous exact familiarity signal is not a truth/semantic judgment.
    const auto cue = input_cue(media, content);
    if (!temporary_.usable()) return recall_cue(cue, RecallScope::unavailable, FamiliarityKey::missing);
    const auto found = temporary_.cues_.find(cue);
    const bool present = found != temporary_.cues_.end() && !found->second.empty();
    const auto local_kind = familiarity_key(present,
        !present && continuation_ && temporary_.find(*continuation_) != nullptr);
    const auto scope = recall_scope(true, local_kind != FamiliarityKey::missing);
    if (scope == RecallScope::temporary) return recall_cue(cue, scope, local_kind);
    const auto main_cue = main_cues_.find(cue);
    const bool main_exact = main_cue != main_cues_.end() && !main_cue->second.empty();
    const auto main_context = continuation_ ? main_.find(*continuation_) : main_.end();
    const auto main_kind = familiarity_key(main_exact,
        main_context != main_.end() && !main_context->second.empty());
    return recall_cue(cue, scope, main_kind);
}
InputRecall ExperienceRouter::recall_cue(const DigestBytes& cue, RecallScope scope, FamiliarityKey kind) const {
    // Recall begins here, before allocating result addresses. No original
    // payloads, connection recovery, storage writes or shuffles run here.
    if (scope == RecallScope::unavailable) throw std::logic_error("temporary experience unavailable");
    InputRecall result(memory_); result.cue_ = cue; result.key_kind_ = kind; result.issuer_ = this;
    result.temporary_ = scope == RecallScope::temporary;
    if (kind == FamiliarityKey::missing) return result;
    if (kind == FamiliarityKey::continuation) {
        const auto candidates = recall(*continuation_);
        for (std::size_t candidate = 0; candidate < candidates.size(); ++candidate) {
            const auto match = candidates.at(candidate);
            const auto count = match.connection->state().experiences().size();
            const auto* active = temporary_.find(match.recalled_head.identity);
            const auto boundary = active ? active->state().experiences().size() : 0;
            for (std::size_t index = 0; index < count; ++index)
                result.matches_.push_back({match, index, boundary});
        }
        return result;
    }
    const auto append = [&](const SessionRuntime& session, const CueReference& reference) {
        const auto* connection = session.find(reference.connection);
        if (!connection) throw std::logic_error("cue refers to an unavailable connection");
        const auto* active = temporary_.find(reference.connection);
        const auto boundary = active ? active->state().experiences().size() : 0;
        result.matches_.push_back({{&session, connection, connection->snapshot()}, reference.original_index, boundary});
    };
    if (scope == RecallScope::temporary) {
        result.temporary_ = true;
        for (const auto& reference : temporary_.cues_.find(cue)->second) append(temporary_, reference);
    } else {
        const auto found = main_cues_.find(cue);
        if (found != main_cues_.end())
            for (const auto& candidate : found->second) append(*candidate.session, candidate.reference);
    }
    return result;
}
ReplayedInput ExperienceRouter::replay(const InputRecall& recalled, std::size_t candidate) const {
    if (recalled.issuer_ != this) throw std::invalid_argument("Recall belongs to a different input route");
    if (candidate >= recalled.matches_.size()) throw std::out_of_range("input recall candidate");
    const auto& selected = recalled.matches_[candidate];
    RecallCandidates bound;
    bound.temporary_ = selected.recalled;
    return ReplayedInput(replay(bound, 0, selected.original_index), selected, this, recalled.cue_);
}
ReEvidenceResult ExperienceRouter::re_evidence(const ReplayedInput& replayed,
    std::uint64_t seed, std::uint64_t step) const {
    if (replayed.issuer_ != this) throw std::invalid_argument("Replay belongs to a different input route");
    const auto& remembered = replayed.match_.recalled;
    const auto identity = remembered.recalled_head.identity;
    const auto* current = temporary_.find(identity);
    const auto& rules = current ? current->rules() : remembered.connection->rules();
    const auto prior = decode_evidence(remembered.connection->rules(), replayed.original_);
    Connection fresh(identity, current ? current->state().strength() : remembered.recalled_head.strength, rules, memory_);
    std::pmr::vector<ExperienceLocation> addresses(&memory_);
    ConnectionHead current_head{};
    if (current) {
        current_head = current->snapshot();
        const auto values = current->state().experiences();
        const auto boundary = replayed.match_.current_observations;
        if (boundary > values.size()) throw std::logic_error("current observation history regressed");
        addresses.reserve(values.size() - boundary);
        for (const auto& value : values.subspan(boundary)) {
            fresh.append(value); addresses.push_back(value.original());
        }
    } else if (replayed.match_.current_observations != 0) {
        throw std::logic_error("current observation history disappeared");
    }
    auto report = fresh.refine(seed, step);
    const auto agreement = compare_replay_evidence(rules, prior.value(), identity,
        report.result().verification().judgment(), step);
    return ReEvidenceResult(std::move(report), agreement, remembered.recalled_head, current_head,
        replayed.location(), replayed.input_cue(), std::move(addresses));
}
RecallCandidates ExperienceRouter::recall(const DigestBytes& identity) const {
    RecallCandidates result;
    const auto* local = temporary_.usable() ? temporary_.find(identity) : nullptr;
    switch (recall_scope(temporary_.usable(), local != nullptr)) {
    case RecallScope::unavailable: throw std::logic_error("temporary experience unavailable");
    case RecallScope::temporary: result.temporary_ = {&temporary_, local, local->snapshot()}; return result;
    case RecallScope::main: break;
    }
    const auto found = main_.find(identity);
    if (found != main_.end()) result.main_ = found->second;
    return result;
}
StoredExperience ExperienceRouter::replay(const RecallCandidates& candidates, std::size_t candidate,
    std::size_t original_index) const {
    const auto selected = candidates.at(candidate);
    const auto* current = selected.session->find(selected.recalled_head.identity);
    if (!current) throw std::logic_error("recalled connection is no longer available");
    const auto head = current->snapshot();
    if (assess_head_publication(&head, selected.recalled_head.record, selected.recalled_head, true)
        != HeadPublication::unchanged)
        throw std::logic_error("connection changed after Recall; recall current experience again");
    auto original = selected.session->replay(selected.connection->state().identity(), original_index);
    continuation_ = selected.recalled_head.identity;
    return original;
}

}  // namespace swegca::vrs
