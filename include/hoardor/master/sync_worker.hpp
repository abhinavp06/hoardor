#pragma once

#include <hoardor/db/database.hpp>
#include <hoardor/file/library.hpp>

#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>

namespace hoardor::master {

struct SyncCallbacks {
    // Both run on the worker thread. They must return quickly, be thread-safe, and
    // not call SyncWorker::wait_idle (that would wait for the callback itself).
    std::function<void(const file::SyncProgress&)> on_progress;
    // The scope that was synced (std::nullopt = everything) and its report.
    std::function<void(std::optional<file::CategoryId>, const file::SyncReport&)> on_finished;
};

// Runs file::Library::sync() on a background thread that hoardor owns
// (features/file_sync.md §4.10, engines/master.md).
class SyncWorker {
public:
    // Opens its own connection to `database_file` and starts the thread. If the stored
    // setting sync_on_startup is true, a global sync is queued right away.
    static file::Result<std::unique_ptr<SyncWorker>> start(const std::filesystem::path& database_file,
                                                           SyncCallbacks callbacks,
                                                           file::MountPointLister mounts = file::list_mount_points);

    // Cancels the running sync, drops the queue, and joins the thread.
    ~SyncWorker();
    SyncWorker(const SyncWorker&) = delete;
    SyncWorker& operator=(const SyncWorker&) = delete;

    // Queues a sync of one category, or of everything (std::nullopt). Returns false
    // and does nothing if the same scope is already queued or running.
    bool request_sync(std::optional<file::CategoryId> category = std::nullopt);
    // Cancels the running sync (it removes nothing) and clears the queue.
    void cancel();
    bool idle() const;
    // Blocks until nothing is queued or running.
    void wait_idle();

private:
    using Scope = std::optional<file::CategoryId>;

    SyncWorker(db::Database database, SyncCallbacks callbacks) : database_(std::move(database)), callbacks_(std::move(callbacks)) {}
    void run(std::stop_token stop);

    db::Database database_;               // used only by the worker thread once it starts
    std::optional<file::Library> library_;
    SyncCallbacks callbacks_;

    mutable std::mutex mutex_;
    std::condition_variable_any changed_;
    std::deque<Scope> queue_;
    std::optional<Scope> running_;
    std::stop_source current_;  // stops the running sync

    std::jthread thread_;  // last: destroyed (stopped and joined) before everything it uses
};

}
