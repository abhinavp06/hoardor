# Master engine (`hoardor::master`)

Status: **Built** (2026-10-01): the first slice, as part of File Sync v1, phase 2. The metadata pass was added on 2026-10-02 (Media library v1, phase 4; tests in `tests/master/metadata_pass_test.cpp`). 8 tests in `tests/master/sync_worker_test.cpp`. They pass 50 repeated runs, and ThreadSanitizer reports no races.

`master` coordinates the other engines (ARCHITECTURE §2). It's also where hoardor owns background execution (`features/file_sync.md` §4.10): engines expose blocking, thread-agnostic functions, and `master` runs them on hoardor's threads.

## 1. First slice: `SyncWorker`

```cpp
namespace hoardor::master {

struct SyncCallbacks {
    // Both run on the worker thread. They must return quickly, must be thread-safe,
    // and must not call blocking SyncWorker functions (wait_idle).
    std::function<void(const file::SyncProgress&)> on_progress;
    std::function<void(std::optional<file::CategoryId>, const file::SyncReport&)> on_finished;
};

class SyncWorker {
public:
    // Opens its own connection to the database file and starts the thread.
    // If the stored setting sync_on_startup is true, it queues a global sync.
    static file::Result<std::unique_ptr<SyncWorker>> start(const std::filesystem::path& database_file,
                                                           SyncCallbacks callbacks,
                                                           file::MountPointLister mounts = file::list_mount_points);
    ~SyncWorker();                       // cancels the running sync, drops the queue, joins the thread

    // Queues a sync of one category, or of everything (std::nullopt). Returns false,
    // and does nothing, if the same scope is already queued or running.
    bool request_sync(std::optional<file::CategoryId> category = std::nullopt);
    void cancel();                       // cancels the running sync and clears the queue
    bool idle() const;
    void wait_idle();                    // blocks until nothing is queued or running
};

}
```

- **Why `std::unique_ptr`:** the thread refers to the worker object, so the worker must never move in memory. Returning it behind a pointer guarantees that.
- **Thread:**
  - one `std::jthread`, the C++20 thread that requests a stop and joins automatically when destroyed
  - a `std::deque` of requested scopes, guarded by a mutex and a `std::condition_variable_any` (it can wait on a stop token)
  - a `std::stop_source` for the sync that's running
- **Connection:** the worker owns its own `db::Database` (opened in `start()`, used only by the worker thread), and its own `file::Library` on it.
- **Duplicate requests:** a request whose scope equals the one running or any one queued is ignored. A global request while a category sync is running is still queued, since it covers more.
- **Not yet:** an I/O priority for the worker, and yielding to playback (`features/file_sync.md` §6, OI-1). They wait for the measurement in OI-1.

## 1a. Reading metadata after a sync (Media library v1, phase 4)

After each requested sync, and unless it was cancelled, `SyncWorker` **queues a metadata job** for that scope (since 2026-10-02; it used to run inside the sync). The job reads the scope's audio and video files that are new, changed, or never read, on the same thread, while the drives are awake (`features/media_listing.md` §4.1).
- **Syncs go ahead of metadata jobs.** A sync request pauses a running pass, which resumes after the sync.
- **`syncing()` / `reading()`** tell them apart.
- **One worker per physical drive** (since 2026-10-02):
  - roots are grouped by `file::device_of` (injectable as `start(…, device_of)` for tests)
  - up to `file::Settings::parallel_devices` drives at once, each worker with its own connection
  - the same split applies to syncs and to metadata passes
  - sync reports keep the roots' order

```cpp
struct MetadataProgress { std::uint64_t done, total; };
struct MetadataReport { std::uint64_t read, failed, skipped; bool cancelled; };
// SyncCallbacks gains: on_metadata_progress (at most every 250 ms) and on_metadata_finished(scope, report).
```

- **The work list:** `audio::Library::pending` / `video::Library::pending`. Settled entries in roots the sync found online.
- **Paths:** resolved once per root. Videos also get their companions (`file::Library::companions(entry, 2)`).
- **Writes** go in short batches (`batch_max_rows` / `batch_max_milliseconds`). Callbacks never run inside a write transaction.
- **A failed read:**
  - If the file or its root has gone (an unplugged drive), it's **skipped**, not recorded, and read on a later pass.
  - Otherwise it's stored as unreadable (`store_error`) and not retried until it changes.
- **`cancel()`** stops the pass. What was read is kept, and the rest stays pending.
- **At the end:** unused artist, genre, and person names are removed.
- **Not run at startup on its own:** that would wake drives nobody asked for. An upgraded library is read on its first sync.

## 2. Tests

`tests/master/sync_worker_test.cpp`:
- a requested sync runs in the background and `on_finished` receives the report
- a duplicate request is rejected while the same scope is running, and a different scope is accepted (made deterministic by blocking inside `on_progress`)
- `cancel()` makes the running sync report Cancelled
- `sync_on_startup = true` queues a global sync at start
- the destructor stops a running sync and joins without hanging
- while the worker is mid-sync, another connection can read (WAL)
