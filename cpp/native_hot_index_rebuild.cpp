#include "native_hot_index_rebuild.hpp"

#include "digest.hpp"
#include "memory_episode.hpp"
#include "observation.hpp"

#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <exception>
#include <filesystem>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace swegca::vrs {
namespace {

constexpr std::size_t rebuild_workers = 16;
constexpr std::size_t queue_limit = 4;

struct PostingWork {
    std::vector<std::string> cues;
    std::vector<std::string> propositions;
    std::vector<std::string> predecessors;
    std::string episode_id;
    std::int64_t sequence;

    // SWEGCA: src/swegca_vrs2/store.py@7536139:145-175
    [[nodiscard]] bool empty() const {
        return cues.empty() && propositions.empty() && predecessors.empty();
    }
};

struct PostingWorker {
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<PostingWork> queue;
    bool done = false;
};

// One posting key always enters one worker in increasing journal sequence order.
// SWEGCA: src/swegca_vrs2/cue_shards.py@c06092a:42-48
// SWEGCA: user@2026-09-22:89-92
std::size_t worker_for(std::string_view cue) {
    const auto digest = sha256_hex(cue);
    const auto digit = digest.front();
    if (digit >= '0' && digit <= '9') return digit - '0';
    if (digit >= 'a' && digit <= 'f') return digit - 'a' + 10;
    throw std::runtime_error("invalid_vrs_cue_digest");
}

}  // namespace

// SWEGCA: src/swegca_vrs2/store.py@7536139:145-175
// SWEGCA: src/swegca_vrs2/cue_shards.py@c06092a:260-360
// SWEGCA: user@2026-09-22:89-92
NativeHotIndexRebuildCount rebuild_native_hot_index_directories(
    const NativeJournal& journal, const ExactJournalDirectory& addresses,
    const HotIndexProjectionLog& headers, NativeCueDirectory& cues,
    NativeCueDirectory& propositions, NativeCueDirectory& successors) {
    if (journal.generation() != addresses.journal_generation() ||
        journal.generation() != headers.journal_generation() ||
        journal.generation() != cues.journal_generation() ||
        journal.generation() != propositions.journal_generation() ||
        journal.generation() != successors.journal_generation())
        throw std::runtime_error("native_hot_index_generation_changed");
    if (std::filesystem::equivalent(cues.directory(), propositions.directory()) ||
        std::filesystem::equivalent(cues.directory(), successors.directory()) ||
        std::filesystem::equivalent(propositions.directory(),
                                    successors.directory()))
        throw std::runtime_error("native_hot_index_directory_alias");
    if (!cues.fresh() || !propositions.fresh() || !successors.fresh())
        throw std::runtime_error("native_hot_index_rebuild_requires_fresh_directories");
    NativeHotIndexRebuildCount count{};
    count.memory = empty_hot_index(journal.identity());
    std::array<PostingWorker, rebuild_workers> workers;
    std::atomic<bool> cancelled{false};
    std::mutex error_mutex;
    std::exception_ptr worker_error;
    const auto fail = [&](std::exception_ptr error) {
        {
            std::lock_guard guard(error_mutex);
            if (!worker_error) worker_error = std::move(error);
        }
        cancelled.store(true);
        for (auto& worker : workers) worker.ready.notify_all();
    };
    std::vector<std::jthread> threads;
    threads.reserve(rebuild_workers);
    std::exception_ptr producer_error;
    try {
        for (std::size_t number = 0; number < rebuild_workers; ++number) {
            threads.emplace_back([&, number] {
                auto& worker = workers[number];
                try {
                    while (true) {
                        std::optional<PostingWork> work;
                        {
                            std::unique_lock lock(worker.mutex);
                            worker.ready.wait(lock, [&] {
                                return cancelled.load() || worker.done ||
                                       !worker.queue.empty();
                            });
                            if (cancelled.load()) return;
                            if (worker.queue.empty()) return;
                            work.emplace(std::move(worker.queue.front()));
                            worker.queue.pop_front();
                        }
                        worker.ready.notify_all();
                        if (!work->cues.empty())
                            cues.put(work->cues, work->episode_id, work->sequence);
                        if (!work->propositions.empty())
                            propositions.put(work->propositions,
                                             work->episode_id, work->sequence);
                        if (!work->predecessors.empty()) {
                            for (const auto& previous : work->predecessors) {
                                if (successors.posting_count(previous,
                                                             work->sequence - 1) != 0)
                                    throw std::runtime_error(
                                        "invalid_source_revision_successor");
                            }
                            successors.put(work->predecessors,
                                           work->episode_id, work->sequence);
                        }
                    }
                } catch (...) {
                    fail(std::current_exception());
                }
            });
        }
        journal.visit_addressed_rows([&](JournalRow&& stored,
                                         const JournalFrameAddress&) {
            if (cancelled.load())
                throw std::runtime_error("native_hot_index_rebuild_cancelled");
            const auto row = observation(Json::parse(stored.body));
            if (row.at("request_id").string() != stored.request_id ||
                sha256_hex(row.canonical()) != stored.fingerprint)
                throw std::runtime_error("stored_observation_integrity_failed");
            const auto identifier = episode_id_from_observation(row);
            ++count.journal_rows;
            const auto original = addresses.find(identifier, stored.sequence);
            const auto header = addresses.find_header(identifier, stored.sequence);
            if (!original || !header || original->sequence > stored.sequence ||
                header->journal_sequence != original->sequence)
                throw std::runtime_error("native_hot_index_original_address_missing");
            if (original->sequence != stored.sequence) {
                ++count.duplicate_observations;
                return;
            }
            const auto projection = headers.read_at(*header, identifier,
                                                     stored.sequence);
            if (projection.pair_snapshot_id != stored.pair_id)
                throw std::runtime_error("native_hot_index_projection_source_changed");
            if (projection.header.supersedes) {
                const auto& previous = *projection.header.supersedes;
                const auto predecessor = addresses.find(previous,
                                                         stored.sequence - 1);
                const auto predecessor_header = addresses.find_header(
                    previous, stored.sequence - 1);
                if (!predecessor || !predecessor_header ||
                    predecessor_header->journal_sequence != predecessor->sequence)
                    throw std::runtime_error("native_hot_index_supersedes_missing");
                const auto prior = headers.read_at(*predecessor_header,
                                                   previous, stored.sequence - 1);
                if (prior.header.source_addresses !=
                        projection.header.source_addresses ||
                    prior.header.revision == projection.header.revision)
                    throw std::runtime_error("invalid_source_revision_successor");
            }
            ++count.distinct_originals;
            count.memory.snapshot_id = next_hot_index_snapshot_id(
                count.memory.snapshot_id, identifier);
            ++count.memory.outcome_counts.at(row.at("outcome").string());
            std::array<PostingWork, rebuild_workers> groups;
            const std::set<std::string> posting_cues(
                projection.posting_cues.begin(), projection.posting_cues.end());
            count.cue_postings += posting_cues.size();
            for (const auto& cue : posting_cues)
                groups[worker_for(cue)].cues.push_back(cue);
            if (projection.header.proposition_id) {
                const auto& proposition = *projection.header.proposition_id;
                groups[worker_for(proposition)].propositions.push_back(proposition);
                ++count.proposition_postings;
            }
            if (projection.header.supersedes) {
                const auto& previous = *projection.header.supersedes;
                groups[worker_for(previous)].predecessors.push_back(previous);
                ++count.successor_postings;
            }
            for (std::size_t number = 0; number < rebuild_workers; ++number) {
                if (groups[number].empty()) continue;
                auto& worker = workers[number];
                std::unique_lock lock(worker.mutex);
                worker.ready.wait(lock, [&] {
                    return cancelled.load() || worker.queue.size() < queue_limit;
                });
                if (cancelled.load())
                    throw std::runtime_error("native_hot_index_rebuild_cancelled");
                groups[number].episode_id = identifier;
                groups[number].sequence = stored.sequence;
                worker.queue.push_back(std::move(groups[number]));
                lock.unlock();
                worker.ready.notify_one();
            }
        });
    } catch (...) {
        producer_error = std::current_exception();
        cancelled.store(true);
    }
    for (auto& worker : workers) {
        {
            std::lock_guard guard(worker.mutex);
            worker.done = true;
        }
        worker.ready.notify_all();
    }
    threads.clear();
    if (worker_error) std::rethrow_exception(worker_error);
    if (producer_error) std::rethrow_exception(producer_error);
    if (count.journal_rows != journal.row_count() ||
        count.distinct_originals + count.duplicate_observations !=
            count.journal_rows)
        throw std::runtime_error("native_hot_index_rebuild_count_changed");
    return count;
}

}  // namespace swegca::vrs
