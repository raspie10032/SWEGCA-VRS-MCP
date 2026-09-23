#include "native_operation_rebuild.hpp"

#include "digest.hpp"
#include "memory_episode.hpp"
#include "native_journal_entry.hpp"

#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace swegca::vrs {
namespace {

constexpr std::size_t rebuild_workers = 16;
constexpr std::size_t queue_limit = 16;

struct OperationWork {
    std::string request_id;
    MainOperation operation;
    std::int64_t sequence;
};

struct OperationWorker {
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<OperationWork> queue;
    bool done = false;
};

// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:311-390
// SWEGCA: user@2026-09-22:89-92
std::size_t worker_for(std::string_view request_id) {
    const auto digest = sha256_hex(request_id);
    const auto digit = digest.front();
    if (digit >= '0' && digit <= '9') return digit - '0';
    if (digit >= 'a' && digit <= 'f') return digit - 'a' + 10;
    throw std::runtime_error("native_operation_digest_invalid");
}

}  // namespace

// SWEGCA: src/swegca_vrs2/store.py@7536139:378-399
// SWEGCA: user@2026-09-22:89-92
NativeOperationRebuildCount rebuild_native_operation_directory(
    const NativeJournal& journal, NativeOperationDirectory& operations) {
    if (journal.generation() != operations.journal_generation())
        throw std::runtime_error("native_operation_generation_changed");
    if (!operations.fresh())
        throw std::runtime_error("native_operation_rebuild_requires_fresh_directory");
    NativeOperationRebuildCount count{};
    std::array<OperationWorker, rebuild_workers> workers;
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
                        std::optional<OperationWork> work;
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
                        operations.put(work->request_id, work->operation,
                                       work->sequence);
                    }
                } catch (...) {
                    fail(std::current_exception());
                }
            });
        }
        journal.visit_addressed_rows([&](JournalRow&& stored,
                                         const JournalFrameAddress&) {
            if (cancelled.load())
                throw std::runtime_error("native_operation_rebuild_cancelled");
            auto entry = parse_native_journal_entry(
                stored.request_id, stored.body, stored.fingerprint);
            ++count.journal_rows;
            if (entry.kind != NativeJournalEntryKind::observation) {
                ++count.provenance_rows;
                return;
            }
            auto identifier = episode_id_from_observation(entry.value);
            ++count.observations;
            auto& worker = workers[worker_for(stored.request_id)];
            std::unique_lock lock(worker.mutex);
            worker.ready.wait(lock, [&] {
                return cancelled.load() || worker.queue.size() < queue_limit;
            });
            if (cancelled.load())
                throw std::runtime_error("native_operation_rebuild_cancelled");
            worker.queue.push_back(OperationWork{
                std::move(stored.request_id),
                MainOperation{std::move(stored.fingerprint),
                              std::move(identifier), std::move(stored.pair_id)},
                stored.sequence});
            lock.unlock();
            worker.ready.notify_one();
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
        count.observations + count.provenance_rows != count.journal_rows)
        throw std::runtime_error("native_operation_rebuild_count_changed");
    return count;
}

}  // namespace swegca::vrs
