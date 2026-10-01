#include <hoardor/master/sync_worker.hpp>

#include <algorithm>

namespace hoardor::master {

file::Result<std::unique_ptr<SyncWorker>> SyncWorker::start(const std::filesystem::path& database_file,
                                                            SyncCallbacks callbacks, file::MountPointLister mounts) {
    auto database = db::Database::open(database_file);
    if (!database) return std::unexpected(file::Error{file::ErrorCode::Database, database.error().message});

    // unique_ptr: the thread refers to the worker, so the worker must never move.
    std::unique_ptr<SyncWorker> worker(new SyncWorker(std::move(*database), std::move(callbacks)));
    auto library = file::Library::open(worker->database_, std::move(mounts));
    if (!library) return std::unexpected(library.error());
    worker->library_.emplace(std::move(*library));

    auto settings = worker->library_->load_settings();
    if (!settings) return std::unexpected(settings.error());
    if (settings->sync_on_startup) worker->queue_.push_back(std::nullopt);

    worker->thread_ = std::jthread([w = worker.get()](std::stop_token stop) { w->run(std::move(stop)); });
    return worker;
}

SyncWorker::~SyncWorker() {
    cancel();
    // thread_ (the last member) is destroyed first: it requests a stop and joins.
}

bool SyncWorker::request_sync(Scope category) {
    {
        std::lock_guard lock(mutex_);
        if (running_ && *running_ == category) return false;
        if (std::find(queue_.begin(), queue_.end(), category) != queue_.end()) return false;
        queue_.push_back(category);
    }
    changed_.notify_all();
    return true;
}

void SyncWorker::cancel() {
    {
        std::lock_guard lock(mutex_);
        queue_.clear();
        current_.request_stop();
    }
    changed_.notify_all();
}

bool SyncWorker::idle() const {
    std::lock_guard lock(mutex_);
    return queue_.empty() && !running_;
}

void SyncWorker::wait_idle() {
    std::unique_lock lock(mutex_);
    changed_.wait(lock, [this] { return queue_.empty() && !running_; });
}

void SyncWorker::run(std::stop_token stop) {
    while (true) {
        Scope scope;
        std::stop_source source;
        {
            std::unique_lock lock(mutex_);
            // Waits for work; returns false when the thread is asked to stop.
            if (!changed_.wait(lock, stop, [this] { return !queue_.empty(); })) return;
            scope = queue_.front();
            queue_.pop_front();
            running_ = scope;
            current_ = std::stop_source();
            source = current_;  // copies share the same stop state
        }
        {
            // Shutting the worker down also stops the running sync.
            std::stop_callback forward(stop, [source]() mutable { source.request_stop(); });
            const file::SyncReport report = library_->sync(scope, source.get_token(), callbacks_.on_progress);
            if (callbacks_.on_finished) callbacks_.on_finished(scope, report);
        }
        {
            std::lock_guard lock(mutex_);
            running_.reset();
        }
        changed_.notify_all();
    }
}

}
