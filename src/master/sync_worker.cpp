#include <hoardor/master/sync_worker.hpp>

#include "file/text.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <thread>

namespace hoardor::master {

file::Result<std::unique_ptr<SyncWorker>> SyncWorker::start(const std::filesystem::path& database_file,
                                                            SyncCallbacks callbacks, file::MountPointLister mounts,
                                                            file::DeviceLookup device_of) {
    auto database = db::Database::open(database_file);
    if (!database) return std::unexpected(file::Error{file::ErrorCode::Database, database.error().message});

    // unique_ptr: the thread refers to the worker, so the worker must never move.
    std::unique_ptr<SyncWorker> worker(new SyncWorker(std::move(*database), std::move(callbacks)));
    worker->database_file_ = database_file;
    worker->mounts_ = mounts;
    worker->device_of_ = std::move(device_of);
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
            const file::SyncReport report = sync(job.scope, source.get_token());
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

namespace {

namespace fs = std::filesystem;

// Roots grouped by the physical drive that holds them, in their original order. Offline or
// unidentifiable roots share one group: they finish at once (offline) or are few.
std::vector<std::vector<std::size_t>> by_device(const std::vector<file::Root>& roots, const file::DeviceLookup& device_of) {
    std::map<std::string, std::vector<std::size_t>> groups;
    std::vector<std::string> order;
    for (std::size_t i = 0; i < roots.size(); ++i) {
        std::string device = device_of ? device_of(file::detail::from_utf8(roots[i].path)) : std::string();
        if (!groups.contains(device)) order.push_back(device);
        groups[device].push_back(i);
    }
    std::vector<std::vector<std::size_t>> out;
    for (const auto& d : order) out.push_back(groups[d]);
    return out;
}

// Runs `work(group)` for every group on up to `threads` threads (the calling thread included).
void for_each_group(std::size_t groups, int threads, const std::function<void(std::size_t)>& work) {
    std::atomic<std::size_t> next{0};
    const auto worker = [&] {
        for (std::size_t g = next++; g < groups; g = next++) work(g);
    };
    std::vector<std::jthread> extra;
    const std::size_t count = std::min<std::size_t>(groups, static_cast<std::size_t>(std::max(1, threads)));
    for (std::size_t i = 1; i < count; ++i) extra.emplace_back(worker);
    worker();
}

}

file::SyncReport SyncWorker::sync(Scope scope, std::stop_token stop) {
    auto settings = library_->load_settings();
    auto roots = library_->roots(scope);
    if (!settings || !roots || roots->size() < 2 || settings->parallel_devices < 2) {
        return library_->sync(scope, stop, callbacks_.on_progress);
    }
    const auto groups = by_device(*roots, device_of_);
    if (groups.size() < 2) return library_->sync(scope, stop, callbacks_.on_progress);

    // One worker per drive, each on its own connection (SQLite: one connection per thread;
    // writers take turns in short batches). Reports keep the roots' order.
    file::SyncReport report;
    report.roots.resize(roots->size());
    const std::size_t count = roots->size();
    for_each_group(groups.size(), settings->parallel_devices, [&](std::size_t g) {
        auto database = db::Database::open(database_file_);
        std::optional<file::Library> library;
        if (database) {
            if (auto lib = file::Library::open(*database, mounts_)) library.emplace(std::move(*lib));
        }
        for (std::size_t index : groups[g]) {
            if (!library) {
                report.roots[index].root_id = (*roots)[index].id;
                report.roots[index].message = "cannot open the library on a second connection";
                continue;
            }
            const auto progress = [&, index](const file::SyncProgress& p) {
                if (!callbacks_.on_progress) return;
                file::SyncProgress global = p;
                global.root_index = index;
                global.root_count = count;
                callbacks_.on_progress(global);
            };
            report.roots[index] = library->sync_root((*roots)[index].id, stop, progress);
        }
    });
    report.cancelled = stop.stop_requested() || std::any_of(report.roots.begin(), report.roots.end(), [](const auto& r) {
                           return r.outcome == file::RootSyncOutcome::Cancelled;
                       });
    return report;
}

MetadataReport SyncWorker::read_metadata(Scope scope, std::stop_token stop) {
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
    const std::uint64_t total = *audio_total + *video_total;
    if (total == 0) return report;

    // Only roots the sync found online are read (pending() also checks that), one worker per drive.
    std::vector<file::Root> online;
    for (const file::Root& r : *roots) {
        if (r.status == file::RootStatus::Online) online.push_back(r);
    }
    const auto groups = by_device(online, device_of_);

    // Shared between the drive workers.
    std::mutex shared;
    std::atomic<std::uint64_t> done{0};
    auto last_report = started;
    const auto progressed = [&] {
        const std::uint64_t now_done = ++done;
        if (!callbacks_.on_metadata_progress) return;
        std::lock_guard lock(shared);
        const auto now = std::chrono::steady_clock::now();
        if (now - last_report < std::chrono::milliseconds(250)) return;
        last_report = now;
        callbacks_.on_metadata_progress(MetadataProgress{now_done, total, elapsed()});
    };
    const auto count = [&](std::uint64_t MetadataReport::*field) {
        std::lock_guard lock(shared);
        ++(report.*field);
    };

    // One drive's roots, on the given connection and libraries.
    const auto read_group = [&](const std::vector<std::size_t>& group, db::Database& db, file::Library& files,
                                audio::Library& tracks, video::Library& videos) {
        std::optional<db::Transaction> tx;
        int rows = 0;
        auto batch_started = std::chrono::steady_clock::now();
        auto last_commit_for_progress = batch_started;
        const auto commit = [&] {
            if (tx) (void)tx->commit();
            tx.reset();
        };
        const auto before_write = [&] {
            if (!tx) {
                if (auto begun = db::Transaction::begin(db)) tx.emplace(std::move(*begun));
                rows = 0;
                batch_started = std::chrono::steady_clock::now();
            }
        };
        const auto after_write = [&] {
            const auto now = std::chrono::steady_clock::now();
            // Short batches, and none left open across a progress report (callbacks never run
            // inside a write transaction, and readers see progress as it's reported).
            if (++rows >= settings->batch_max_rows || now - batch_started >= std::chrono::milliseconds(settings->batch_max_milliseconds) ||
                now - last_commit_for_progress >= std::chrono::milliseconds(250)) {
                commit();
                last_commit_for_progress = now;
            }
            progressed();
        };

        for (std::size_t index : group) {
            const file::Root& root = online[index];
            const fs::path base = file::detail::from_utf8(root.path);
            bool lost = false;
            // A failed read: an unreadable file, or the file or its drive went away (left alone).
            const auto gone = [&](const fs::path& path) {
                std::error_code ec;
                if (!fs::is_directory(base, ec)) lost = true;
                return lost || !fs::exists(path, ec);
            };

            for (audio::EntryId after = 0; !stop.stop_requested() && !lost;) {
                auto page = tracks.pending(scope, after, 200, root.id);
                if (!page || page->empty()) break;
                for (const auto& p : *page) {
                    after = p.entry_id;
                    if (stop.stop_requested() || lost) break;
                    const fs::path path = base / file::detail::from_utf8(p.relative_path);
                    auto info = audio::read(path);
                    if (!info && gone(path)) {
                        count(&MetadataReport::skipped);
                        continue;
                    }
                    before_write();
                    if (info) {
                        if (tracks.store(p.entry_id, p.size, p.mtime_ns, *info)) count(&MetadataReport::read);
                    } else if (tracks.store_error(p.entry_id, p.size, p.mtime_ns, info.error().message)) {
                        count(&MetadataReport::failed);
                    }
                    after_write();
                }
            }

            for (video::EntryId after = 0; !stop.stop_requested() && !lost;) {
                auto page = videos.pending(scope, after, 200, root.id);
                if (!page || page->empty()) break;
                for (const auto& p : *page) {
                    after = p.entry_id;
                    if (stop.stop_requested() || lost) break;
                    const fs::path path = base / file::detail::from_utf8(p.relative_path);
                    std::vector<fs::path> companion_paths;
                    std::vector<file::EntryId> companion_ids;
                    const auto prefixes = video::companion_prefixes(path);
                    if (auto companions = files.companions(p.entry_id, 2, 200, prefixes)) {
                        for (const file::Entry& c : *companions) {
                            companion_paths.push_back(base / file::detail::from_utf8(c.relative_path));
                            companion_ids.push_back(c.id);
                        }
                    }
                    auto info = video::read(path, companion_paths);
                    if (!info && gone(path)) {
                        count(&MetadataReport::skipped);
                        continue;
                    }
                    before_write();
                    if (info) {
                        const file::EntryId poster = info->poster_index >= 0 ? companion_ids[static_cast<std::size_t>(info->poster_index)] : 0;
                        if (videos.store(p.entry_id, p.size, p.mtime_ns, *info, poster)) count(&MetadataReport::read);
                    } else if (videos.store_error(p.entry_id, p.size, p.mtime_ns, info.error().message)) {
                        count(&MetadataReport::failed);
                    }
                    after_write();
                }
            }
        }
        commit();
    };

    if (groups.size() < 2 || settings->parallel_devices < 2) {
        std::vector<std::size_t> all;
        for (const auto& g : groups) all.insert(all.end(), g.begin(), g.end());
        read_group(all, database_, *library_, *audio_, *video_);
    } else {
        for_each_group(groups.size(), settings->parallel_devices, [&](std::size_t g) {
            auto database = db::Database::open(database_file_);
            if (!database) return;
            auto files = file::Library::open(*database, mounts_);
            auto tracks = audio::Library::open(*database);
            auto videos = video::Library::open(*database);
            if (!files || !tracks || !videos) return;
            read_group(groups[g], *database, *files, *tracks, *videos);
        });
    }

    report.cancelled = stop.stop_requested();
    report.elapsed_ms = elapsed();
    (void)audio_->remove_unused_names();
    (void)video_->remove_unused_names();
    if (callbacks_.on_metadata_progress) callbacks_.on_metadata_progress(MetadataProgress{done.load(), total, report.elapsed_ms});
    return report;
}

}
