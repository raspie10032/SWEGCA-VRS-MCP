#include "exact_journal_rebuild.hpp"

#include "digest.hpp"
#include "observation.hpp"

#include <array>
#include <atomic>
#include <cstddef>
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
constexpr std::size_t queue_limit = 64;

struct AddressWork {
    std::string episode_id;
    OriginalJournalAddress address;
};

struct AddressWorker {
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<AddressWork> queue;
    bool done = false;
    std::uint64_t distinct = 0;
    std::uint64_t duplicates = 0;
};

// All appearances of one original ID enter the same worker in journal order.
// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:311-390
std::size_t worker_for(std::string_view episode_id) {
    if (episode_id.size() != 71 || !episode_id.starts_with("memory:"))
        throw std::runtime_error("invalid_exact_experience_address");
    constexpr std::string_view digits = "0123456789abcdef";
    const auto worker = digits.find(episode_id[7]);
    if (worker == std::string_view::npos)
        throw std::runtime_error("invalid_exact_experience_address");
    return worker;
}

}  // namespace

// SWEGCA: src/swegca_vrs2/store.py@7536139:340-349
// SWEGCA: src/swegca_vrs2/exact_replay.py@c06092a:496-609
// SWEGCA: user@2026-09-22:89-92
ExactJournalRebuildCount rebuild_exact_journal_directory(
    const NativeJournal& journal, ExactJournalDirectory& addresses) {
    if (journal.generation() != addresses.journal_generation())
        throw std::runtime_error("exact_journal_generation_changed");
    if (!addresses.fresh())
        throw std::runtime_error("exact_journal_rebuild_requires_fresh_directory");
    ExactJournalRebuildCount count{};
    std::array<AddressWorker, rebuild_workers> workers;
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
                        std::optional<AddressWork> work;
                        {
                            std::unique_lock lock(worker.mutex);
                            worker.ready.wait(lock, [&] {
                                return cancelled.load() || worker.done || !worker.queue.empty();
                            });
                            if (cancelled.load()) return;
                            if (worker.queue.empty()) return;
                            work.emplace(std::move(worker.queue.front()));
                            worker.queue.pop_front();
                        }
                        worker.ready.notify_all();
                        const auto previous = addresses.find(
                            work->episode_id, work->address.sequence - 1);
                        if (previous) {
                            ++worker.duplicates;
                        } else {
                            addresses.put(work->episode_id, work->address);
                            ++worker.distinct;
                        }
                    }
                } catch (...) {
                    fail(std::current_exception());
                }
            });
        }
        journal.visit_addressed_rows([&](JournalRow&& stored,
                                         const JournalFrameAddress& frame) {
            if (cancelled.load())
                throw std::runtime_error("exact_journal_rebuild_cancelled");
            const auto row = observation(Json::parse(stored.body));
            if (row.at("request_id").string() != stored.request_id ||
                sha256_hex(row.canonical()) != stored.fingerprint)
                throw std::runtime_error("stored_observation_integrity_failed");
            auto episode = episode_from_observation(row);
            ++count.journal_rows;
            auto& worker = workers[worker_for(episode.episode_id)];
            std::unique_lock lock(worker.mutex);
            worker.ready.wait(lock, [&] {
                return cancelled.load() || worker.queue.size() < queue_limit;
            });
            if (cancelled.load())
                throw std::runtime_error("exact_journal_rebuild_cancelled");
            worker.queue.push_back(AddressWork{
                std::move(episode.episode_id), OriginalJournalAddress{frame, stored.sequence}});
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
    for (const auto& worker : workers) {
        count.distinct_originals += worker.distinct;
        count.duplicate_observations += worker.duplicates;
    }
    if (count.journal_rows != journal.row_count())
        throw std::runtime_error("exact_journal_rebuild_row_count_changed");
    if (count.distinct_originals + count.duplicate_observations != count.journal_rows)
        throw std::runtime_error("exact_journal_rebuild_count_changed");
    return count;
}

}  // namespace swegca::vrs
