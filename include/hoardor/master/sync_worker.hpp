#pragma once

#include <hoardor/audio/audio.hpp>
#include <hoardor/db/database.hpp>
#include <hoardor/file/library.hpp>
#include <hoardor/video/video.hpp>

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

// Reading metadata after a sync (features/media_listing.md §4.1).
struct MetadataProgress {
    std::uint64_t done = 0;        // files read (or failed) so far in this pass
    std::uint64_t total = 0;       // files that needed reading when the pass started
    std::int64_t elapsed_ms = 0;   // since the pass started (done / elapsed = files per second)
};

struct MetadataReport {
    std::uint64_t read = 0;     // stored
    std::uint64_t failed = 0;   // unreadable: recorded, not retried until the file changes
    std::uint64_t skipped = 0;  // gone or its drive went away: left for a later pass
    bool cancelled = false;
    std::int64_t elapsed_ms = 0;
};

struct SyncCallbacks {
    // Both run on the worker thread. They must return quickly, be thread-safe, and
    // not call SyncWorker::wait_idle (that would wait for the callback itself).
    std::function<void(const file::SyncProgress&)> on_progress;
    // The scope that was synced (std::nullopt = everything) and its report.
    std::function<void(std::optional<file::CategoryId>, const file::SyncReport&)> on_finished;
    // After a sync, reading the same scope's new, changed, or never-read audio and video files
    // is queued as its own job (it runs while the drives are awake, after any queued sync, and
    // a new sync request pauses it). Same thread rules as above. A paused pass resumes later
    // without an on_metadata_finished in between.
    std::function<void(const MetadataProgress&)> on_metadata_progress;
    std::function<void(std::optional<file::CategoryId>, const MetadataReport&)> on_metadata_finished;
};

// Runs file::Library::sync() on a background thread that hoardor owns
// (features/file_sync.md §4.10, engines/master.md), and reading metadata (audio and video
// engines) as a separate, lower-priority job: a sync is never kept waiting by it.
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

    // Queues a sync of one category, or of everything (std::nullopt), ahead of any metadata
    // job; a running metadata job is paused for it. Returns false and does nothing if the
    // same scope's sync is already queued or running.
    bool request_sync(std::optional<file::CategoryId> category = std::nullopt);
    // Cancels the running job (a sync removes nothing; a pass keeps what it read) and clears the queue.
    void cancel();
    bool idle() const;     // nothing queued or running
    bool syncing() const;  // a sync is queued or running
    bool reading() const;  // a metadata job is queued or running
    // Blocks until nothing is queued or running.
    void wait_idle();

private:
    using Scope = std::optional<file::CategoryId>;
    enum class Kind { Sync, Metadata };
    struct Job {
        Kind kind;
        Scope scope;
        bool operator==(const Job&) const = default;
    };

    SyncWorker(db::Database database, SyncCallbacks callbacks) : database_(std::move(database)), callbacks_(std::move(callbacks)) {}
    void run(std::stop_token stop);
    MetadataReport read_metadata(Scope scope, std::stop_token stop);

    db::Database database_;               // used only by the worker thread once it starts
    std::optional<file::Library> library_;
    std::optional<audio::Library> audio_;
    std::optional<video::Library> video_;
    SyncCallbacks callbacks_;

    mutable std::mutex mutex_;
    std::condition_variable_any changed_;
    std::deque<Job> queue_;           // syncs first, then metadata jobs
    std::optional<Job> running_;
    std::stop_source current_;        // stops the running job
    bool paused_ = false;             // the running metadata job was stopped for a sync: queue it again

    std::jthread thread_;  // last: destroyed (stopped and joined) before everything it uses
};

}
