#include <hoardor/master/sync_worker.hpp>

#include "file/text.hpp"

#include <algorithm>
#include <chrono>
#include <map>

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
    auto audio = audio::Library::open(worker->database_);
    if (!audio) return std::unexpected(file::Error{file::ErrorCode::Database, audio.error().message});
    worker->audio_.emplace(std::move(*audio));
    auto video = video::Library::open(worker->database_);
    if (!video) return std::unexpected(file::Error{file::ErrorCode::Database, video.error().message});
    worker->video_.emplace(std::move(*video));

    auto settings = worker->library_->load_settings();
    if (!settings) return std::unexpected(settings.error());
    if (settings->sync_on_startup) worker->queue_.push_back(Job{Kind::Sync, std::nullopt});

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
        const Job job{Kind::Sync, category};
        if (running_ == job) return false;
        if (std::find(queue_.begin(), queue_.end(), job) != queue_.end()) return false;
        // Ahead of every metadata job.
        const auto first_metadata = std::find_if(queue_.begin(), queue_.end(), [](const Job& j) { return j.kind == Kind::Metadata; });
        queue_.insert(first_metadata, job);
        if (running_ && running_->kind == Kind::Metadata) {
            paused_ = true;
            current_.request_stop();
        }
    }
    changed_.notify_all();
    return true;
}

void SyncWorker::cancel() {
    {
        std::lock_guard lock(mutex_);
        queue_.clear();
        paused_ = false;
        current_.request_stop();
    }
    changed_.notify_all();
}

bool SyncWorker::idle() const {
    std::lock_guard lock(mutex_);
    return queue_.empty() && !running_;
}

bool SyncWorker::syncing() const {
    std::lock_guard lock(mutex_);
    if (running_ && running_->kind == Kind::Sync) return true;
    return std::any_of(queue_.begin(), queue_.end(), [](const Job& j) { return j.kind == Kind::Sync; });
}

bool SyncWorker::reading() const {
    std::lock_guard lock(mutex_);
    if (running_ && running_->kind == Kind::Metadata) return true;
    return std::any_of(queue_.begin(), queue_.end(), [](const Job& j) { return j.kind == Kind::Metadata; });
}

void SyncWorker::wait_idle() {
    std::unique_lock lock(mutex_);
    changed_.wait(lock, [this] { return queue_.empty() && !running_; });
}

void SyncWorker::run(std::stop_token stop) {
    while (true) {
        Job job;
        std::stop_source source;
        {
            std::unique_lock lock(mutex_);
            // Waits for work; returns false when the thread is asked to stop.
            if (!changed_.wait(lock, stop, [this] { return !queue_.empty(); })) return;
            job = queue_.front();
            queue_.pop_front();
            running_ = job;
            paused_ = false;
            current_ = std::stop_source();
            source = current_;  // copies share the same stop state
        }
        // Shutting the worker down also stops the running job.
        std::stop_callback forward(stop, [source]() mutable { source.request_stop(); });
        if (job.kind == Kind::Sync) {
            const file::SyncReport report = library_->sync(job.scope, source.get_token(), callbacks_.on_progress);
            if (callbacks_.on_finished) callbacks_.on_finished(job.scope, report);
            if (!report.cancelled && !source.stop_requested()) {
                std::lock_guard lock(mutex_);
                // One pass per scope; a pass over everything covers a category's.
                const bool covered = std::any_of(queue_.begin(), queue_.end(), [&](const Job& j) {
                    return j.kind == Kind::Metadata && (!j.scope || j.scope == job.scope);
                });
                if (!covered) queue_.push_back(Job{Kind::Metadata, job.scope});
            }
        } else {
            const MetadataReport report = read_metadata(job.scope, source.get_token());
            bool paused = false;
            {
                std::lock_guard lock(mutex_);
                paused = paused_ && !stop.stop_requested();
                if (paused) queue_.push_back(job);  // after the sync that paused it
            }
            if (!paused && callbacks_.on_metadata_finished) callbacks_.on_metadata_finished(job.scope, report);
        }
        {
            std::lock_guard lock(mutex_);
            running_.reset();
        }
        changed_.notify_all();
    }
}

MetadataReport SyncWorker::read_metadata(Scope scope, std::stop_token stop) {
    namespace fs = std::filesystem;
    MetadataReport report;
    const auto started = std::chrono::steady_clock::now();
    const auto elapsed = [&] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    };
    auto settings = library_->load_settings();
    auto roots = library_->roots(scope);
    auto audio_total = audio_->pending_count(scope);
    auto video_total = video_->pending_count(scope);
    if (!settings || !roots || !audio_total || !video_total) return report;

    // Only roots the sync just found online are read (pending() also checks that).
    std::map<file::RootId, fs::path> online;
    for (const file::Root& r : *roots) {
        if (r.status == file::RootStatus::Online) online[r.id] = file::detail::from_utf8(r.path);
    }
    MetadataProgress progress{0, *audio_total + *video_total};
    if (progress.total == 0) return report;

    // Short write batches, like the sync, so other connections never wait long.
    std::optional<db::Transaction> tx;
    int rows = 0;
    auto batch_started = std::chrono::steady_clock::now();
    auto last_report = batch_started;
    const auto commit = [&] {
        if (tx) (void)tx->commit();
        tx.reset();
    };
    const auto before_write = [&] {
        if (!tx) {
            if (auto begun = db::Transaction::begin(database_)) tx.emplace(std::move(*begun));
            rows = 0;
            batch_started = std::chrono::steady_clock::now();
        }
    };
    const auto after_write = [&] {
        ++progress.done;
        const auto now = std::chrono::steady_clock::now();
        if (++rows >= settings->batch_max_rows || now - batch_started >= std::chrono::milliseconds(settings->batch_max_milliseconds)) {
            commit();
        }
        if (callbacks_.on_metadata_progress && now - last_report >= std::chrono::milliseconds(250)) {
            commit();  // callbacks never run inside a write transaction
            last_report = now;
            progress.elapsed_ms = elapsed();
            callbacks_.on_metadata_progress(progress);
        }
    };
    // A failed read: unreadable file, or the file or its drive went away (then it's left alone).
    std::map<file::RootId, bool> lost;
    const auto gone = [&](file::RootId root, const fs::path& path) {
        std::error_code ec;
        if (!fs::is_directory(online[root], ec)) lost[root] = true;
        return lost[root] || !fs::exists(path, ec);
    };

    // Audio.
    for (audio::EntryId after = 0; !stop.stop_requested();) {
        auto page = audio_->pending(scope, after, 200);
        if (!page || page->empty()) break;
        for (const auto& p : *page) {
            after = p.entry_id;
            if (stop.stop_requested()) break;
            if (lost[p.root_id] || !online.contains(p.root_id)) {
                ++report.skipped;
                continue;
            }
            const fs::path path = online[p.root_id] / file::detail::from_utf8(p.relative_path);
            auto info = audio::read(path);
            if (!info && gone(p.root_id, path)) {
                ++report.skipped;
                continue;
            }
            before_write();
            if (info) {
                if (audio_->store(p.entry_id, p.size, p.mtime_ns, *info)) ++report.read;
            } else if (audio_->store_error(p.entry_id, p.size, p.mtime_ns, info.error().message)) {
                ++report.failed;
            }
            after_write();
        }
    }

    // Video, with each file's .nfo and images (up to the show's folder).
    for (video::EntryId after = 0; !stop.stop_requested();) {
        auto page = video_->pending(scope, after, 200);
        if (!page || page->empty()) break;
        for (const auto& p : *page) {
            after = p.entry_id;
            if (stop.stop_requested()) break;
            if (lost[p.root_id] || !online.contains(p.root_id)) {
                ++report.skipped;
                continue;
            }
            const fs::path base = online[p.root_id];
            const fs::path path = base / file::detail::from_utf8(p.relative_path);
            std::vector<fs::path> companion_paths;
            std::vector<file::EntryId> companion_ids;
            if (auto companions = library_->companions(p.entry_id, 2)) {
                for (const file::Entry& c : *companions) {
                    companion_paths.push_back(base / file::detail::from_utf8(c.relative_path));
                    companion_ids.push_back(c.id);
                }
            }
            auto info = video::read(path, companion_paths);
            if (!info && gone(p.root_id, path)) {
                ++report.skipped;
                continue;
            }
            before_write();
            if (info) {
                const file::EntryId poster = info->poster_index >= 0 ? companion_ids[static_cast<std::size_t>(info->poster_index)] : 0;
                if (video_->store(p.entry_id, p.size, p.mtime_ns, *info, poster)) ++report.read;
            } else if (video_->store_error(p.entry_id, p.size, p.mtime_ns, info.error().message)) {
                ++report.failed;
            }
            after_write();
        }
    }
    commit();
    report.cancelled = stop.stop_requested();
    report.elapsed_ms = elapsed();
    progress.elapsed_ms = report.elapsed_ms;
    (void)audio_->remove_unused_names();
    (void)video_->remove_unused_names();
    if (callbacks_.on_metadata_progress) callbacks_.on_metadata_progress(progress);
    return report;
}

}
