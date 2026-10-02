// Sync: one root's scan reconciled against the database (features/file_sync.md §4.6).

#include <hoardor/file/library.hpp>

#include "file/library_internal.hpp"
#include "file/text.hpp"

#include <algorithm>
#include <chrono>
#include <unordered_set>

namespace hoardor::file {

namespace fs = std::filesystem;
using detail::database_error;

namespace {

std::int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

Result<std::int64_t> count_entries(db::Database& db, RootId id, std::string_view extra_condition = {},
                                   std::int64_t generation = 0) {
    auto st = db.prepare("SELECT COUNT(*) FROM file_entries WHERE root_id = ?" + std::string(extra_condition));
    if (!st) return std::unexpected(database_error(st.error()));
    st->bind(1, id);
    if (!extra_condition.empty()) st->bind(2, generation);
    auto row = st->step();
    if (!row) return std::unexpected(database_error(row.error()));
    return st->column_int64(0);
}

// The statements one sync reuses for every file, prepared once.
struct Statements {
    db::Statement touch_unchanged;
    db::Statement find;
    db::Statement insert;
    db::Statement update_changed;
    db::Statement update_seen;
    db::Statement touch_subtree;
    db::Statement touch_one;
    db::Statement record_error;

    static Result<Statements> prepare(db::Database& db) {
        // The common case in one statement: stamp the file as seen only if nothing about it
        // changed (and it isn't just settling). 0 rows changed -> take the full path.
        auto touch_unchanged = db.prepare("UPDATE file_entries SET seen_generation = ? WHERE root_id = ? AND path_key = ? "
                                          "AND size = ? AND mtime_ns = ? AND kind = ? AND relative_path = ? "
                                          "AND unsettled = ?");
        auto find = db.prepare("SELECT id, size, mtime_ns, kind, relative_path, unsettled FROM file_entries "
                               "WHERE root_id = ? AND path_key = ?");
        auto insert = db.prepare("INSERT INTO file_entries (root_id, relative_path, path_key, size, mtime_ns, kind, "
                                 "unsettled, seen_generation, changed_generation, added_ns) "
                                 "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
        auto update_changed = db.prepare("UPDATE file_entries SET relative_path = ?, size = ?, mtime_ns = ?, kind = ?, "
                                         "unsettled = ?, seen_generation = ?, changed_generation = ? WHERE id = ?");
        auto update_seen = db.prepare("UPDATE file_entries SET seen_generation = ?, unsettled = ? WHERE id = ?");
        // Every key under "dir/": '/' is 0x2F and '0' is 0x30, so [dir/, dir0) is exactly that subtree.
        auto touch_subtree = db.prepare("UPDATE file_entries SET seen_generation = ? "
                                        "WHERE root_id = ? AND path_key >= ? AND path_key < ?");
        auto touch_one = db.prepare("UPDATE file_entries SET seen_generation = ? WHERE root_id = ? AND path_key = ?");
        auto record_error = db.prepare("INSERT INTO file_scan_errors (root_id, relative_path, is_directory, message, "
                                       "generation) VALUES (?, ?, ?, ?, ?)");
        for (auto* st : {&touch_unchanged, &find, &insert, &update_changed, &update_seen, &touch_subtree, &touch_one, &record_error}) {
            if (!*st) return std::unexpected(database_error(st->error()));
        }
        return Statements{std::move(*touch_unchanged), std::move(*find),          std::move(*insert),    std::move(*update_changed),
                          std::move(*update_seen),   std::move(*touch_subtree), std::move(*touch_one),
                          std::move(*record_error)};
    }
};

Result<void> run(db::Statement& st) {
    auto r = st.run();
    st.reset();
    if (!r) return std::unexpected(database_error(r.error()));
    return {};
}

}

RootSyncReport Library::sync_root(RootId id, std::stop_token stop, const ProgressCallback& progress) {
    return sync_one(id, std::move(stop), progress, 0, 1);
}

SyncReport Library::sync(std::optional<CategoryId> category, std::stop_token stop, const ProgressCallback& progress) {
    SyncReport report;
    auto targets = roots(category);
    if (!targets) {
        RootSyncReport failed;
        failed.message = targets.error().message;
        report.roots.push_back(std::move(failed));
        return report;
    }
    for (std::size_t i = 0; i < targets->size(); ++i) {
        if (stop.stop_requested()) {
            report.cancelled = true;
            break;
        }
        report.roots.push_back(sync_one((*targets)[i].id, stop, progress, i, targets->size()));
        if (report.roots.back().outcome == RootSyncOutcome::Cancelled) report.cancelled = true;
    }
    return report;
}

RootSyncReport Library::sync_one(RootId id, std::stop_token stop, const ProgressCallback& progress, std::size_t index,
                                 std::size_t count) {
    RootSyncReport report;
    report.root_id = id;
    const auto finish = [&](RootSyncOutcome outcome, std::string message) {
        report.outcome = outcome;
        report.message = std::move(message);
        if (outcome == RootSyncOutcome::Offline) (void)detail::set_root_status(*db_, id, RootStatus::Offline);
        return report;
    };

    auto root = this->root(id);
    if (!root) return finish(RootSyncOutcome::Failed, root.error().message);
    if (stop.stop_requested()) return finish(RootSyncOutcome::Cancelled, "cancelled before it started");

    // 1. Settings, filtered to the kinds this root's category accepts.
    auto settings = load_settings();
    if (!settings) return finish(RootSyncOutcome::Failed, settings.error().message);
    auto all_categories = categories();
    if (!all_categories) return finish(RootSyncOutcome::Failed, all_categories.error().message);
    const auto category = std::find_if(all_categories->begin(), all_categories->end(),
                                       [&](const Category& c) { return c.id == root->category_id; });
    std::unordered_set<FileKind> accepted;
    if (category != all_categories->end()) accepted.insert(category->kinds.begin(), category->kinds.end());
    std::erase_if(settings->extension_kinds, [&](const auto& pair) { return !accepted.contains(pair.second); });

    // 2. Find the storage.
    auto resolution = resolve_root(*root);
    if (!resolution) return finish(RootSyncOutcome::Failed, resolution.error().message);
    report.relocated = resolution->relocated;
    if (!resolution->online) return finish(RootSyncOutcome::Offline, resolution->reason);

    // 3. A new generation; every file this sync sees is stamped with it.
    const std::int64_t generation = root->generation + 1;
    report.generation = generation;
    auto prior_count = count_entries(*db_, id);
    if (!prior_count) return finish(RootSyncOutcome::Failed, prior_count.error().message);

    auto scanner = Scanner::open(detail::from_utf8(root->path), *settings);
    if (!scanner) {
        if (scanner.error() == std::errc::no_such_file_or_directory) {
            return finish(RootSyncOutcome::Offline, "the folder disappeared");
        }
        return finish(RootSyncOutcome::Failed, "cannot read the folder: " + scanner.error().message());
    }
    auto statements = Statements::prepare(*db_);
    if (!statements) return finish(RootSyncOutcome::Failed, statements.error().message);
    auto& st = *statements;

    // 4. Stream the scan through short write transactions.
    std::optional<db::Transaction> tx;
    int rows_in_batch = 0;
    auto batch_started = std::chrono::steady_clock::now();
    const auto commit = [&]() -> Result<void> {
        if (!tx) return {};
        auto r = tx->commit();
        tx.reset();
        if (!r) return std::unexpected(database_error(r.error()));
        return {};
    };
    const auto report_progress = [&] {
        if (progress) progress(SyncProgress{index, count, id, scanner->progress()});
    };

    const std::int64_t now = now_ns();
    const std::int64_t window = settings->settle_window_seconds * 1'000'000'000;
    std::uint64_t files_seen = 0;
    bool root_lost = false;
    bool cancelled = false;

    report_progress();
    while (true) {
        if (stop.stop_requested()) {
            cancelled = true;
            break;
        }
        auto item = scanner->next();
        if (!item) break;
        if (!tx) {
            auto begun = db::Transaction::begin(*db_);
            if (!begun) return finish(RootSyncOutcome::Failed, begun.error().message);
            tx.emplace(std::move(*begun));
            rows_in_batch = 0;
            batch_started = std::chrono::steady_clock::now();
        }

        if (*item) {
            const ScannedFile& file = **item;
            const std::string key = detail::path_key(file.relative_path, root->case_sensitive);
            // Unsettled: modified within the window, in either direction, so a file dated far in the
            // future (a wrong camera clock) isn't unsettled forever.
            const bool unsettled = file.mtime_ns > now - window && file.mtime_ns < now + window;
            const auto size = static_cast<std::int64_t>(file.size);
            const auto kind = static_cast<std::int64_t>(file.kind);

            st.touch_unchanged.bind(1, generation)
                .bind(2, id)
                .bind(3, std::string_view(key))
                .bind(4, size)
                .bind(5, file.mtime_ns)
                .bind(6, kind)
                .bind(7, std::string_view(file.relative_path))
                .bind(8, unsettled ? 1 : 0);
            if (auto r = run(st.touch_unchanged); !r) return finish(RootSyncOutcome::Failed, r.error().message);
            const bool unchanged = db_->changes() == 1;

            bool found = false;
            if (!unchanged) {
                st.find.bind(1, id).bind(2, std::string_view(key));
                auto step = st.find.step();
                if (!step) return finish(RootSyncOutcome::Failed, step.error().message);
                found = *step;
            }
            if (unchanged) {
                ++report.unchanged;
            } else if (!found) {
                st.find.reset();
                st.insert.bind(1, id)
                    .bind(2, std::string_view(file.relative_path))
                    .bind(3, std::string_view(key))
                    .bind(4, size)
                    .bind(5, file.mtime_ns)
                    .bind(6, kind)
                    .bind(7, unsettled ? 1 : 0)
                    .bind(8, generation)
                    .bind(9, generation)
                    .bind(10, now);
                auto r = st.insert.run();
                st.insert.reset();
                if (!r) {
                    if (!detail::is_constraint(r.error())) return finish(RootSyncOutcome::Failed, r.error().message);
                    // Two names differing only in case on a root marked case-insensitive.
                    st.record_error.bind(1, id)
                        .bind(2, std::string_view(file.relative_path))
                        .bind(3, 0)
                        .bind(4, std::string_view("another file has the same name ignoring case"))
                        .bind(5, generation);
                    if (auto e = run(st.record_error); !e) return finish(RootSyncOutcome::Failed, e.error().message);
                    ++report.errors;
                } else {
                    ++report.added;
                }
            } else {
                const EntryId entry_id = st.find.column_int64(0);
                const bool changed = st.find.column_int64(1) != size || st.find.column_int64(2) != file.mtime_ns ||
                                     st.find.column_int64(3) != kind || st.find.column_text(4) != file.relative_path ||
                                     (st.find.column_int64(5) != 0 && !unsettled);  // just settled: now worth reading
                st.find.reset();
                if (changed) {
                    st.update_changed.bind(1, std::string_view(file.relative_path))
                        .bind(2, size)
                        .bind(3, file.mtime_ns)
                        .bind(4, kind)
                        .bind(5, unsettled ? 1 : 0)
                        .bind(6, generation)
                        .bind(7, generation)
                        .bind(8, entry_id);
                    if (auto r = run(st.update_changed); !r) return finish(RootSyncOutcome::Failed, r.error().message);
                    ++report.modified;
                } else {
                    st.update_seen.bind(1, generation).bind(2, unsettled ? 1 : 0).bind(3, entry_id);
                    if (auto r = run(st.update_seen); !r) return finish(RootSyncOutcome::Failed, r.error().message);
                    ++report.unchanged;
                }
            }
            if (unsettled) ++report.unsettled;
            ++files_seen;
        } else {
            const ScanError& error = item->error();
            ++report.errors;
            st.record_error.bind(1, id)
                .bind(2, std::string_view(error.relative_path))
                .bind(3, error.is_directory ? 1 : 0)
                .bind(4, std::string_view(error.code.message()))
                .bind(5, generation);
            if (auto r = run(st.record_error); !r) return finish(RootSyncOutcome::Failed, r.error().message);
            if (error.relative_path.empty()) {
                root_lost = true;
                break;
            }
            // What couldn't be read is unknown, not deleted: keep it by marking it seen.
            const std::string key = detail::path_key(error.relative_path, root->case_sensitive);
            if (error.is_directory) {
                st.touch_subtree.bind(1, generation).bind(2, id).bind(3, key + "/").bind(4, key + "0");
                if (auto r = run(st.touch_subtree); !r) return finish(RootSyncOutcome::Failed, r.error().message);
            } else {
                st.touch_one.bind(1, generation).bind(2, id).bind(3, std::string_view(key));
                if (auto r = run(st.touch_one); !r) return finish(RootSyncOutcome::Failed, r.error().message);
            }
        }

        ++rows_in_batch;
        const auto elapsed = std::chrono::steady_clock::now() - batch_started;
        const bool progress_due = progress && *item && files_seen % settings->progress_interval_files == 0;
        if (rows_in_batch >= settings->batch_max_rows || elapsed >= std::chrono::milliseconds(settings->batch_max_milliseconds) ||
            progress_due) {
            if (auto r = commit(); !r) return finish(RootSyncOutcome::Failed, r.error().message);
        }
        if (progress_due) report_progress();  // after the commit: callbacks never hold the write lock
    }
    if (auto r = commit(); !r) return finish(RootSyncOutcome::Failed, r.error().message);
    report_progress();

    if (cancelled) return finish(RootSyncOutcome::Cancelled, "cancelled; nothing was removed");
    if (root_lost) return finish(RootSyncOutcome::Offline, "the folder became unavailable during the sync; nothing was removed");

    // 5. Still the same storage at the end?
    auto after = resolve_root(*root);
    if (!after) return finish(RootSyncOutcome::Failed, after.error().message);
    if (!after->online) return finish(RootSyncOutcome::Offline, after->reason);

    // 6. Empty-root guard: probably an empty mount point left by an unmounted drive.
    if (files_seen == 0 && *prior_count > 0) {
        return finish(RootSyncOutcome::Offline, "the folder is empty but had " + std::to_string(*prior_count) +
                                                    " files; treating it as unmounted, nothing was removed");
    }

    // 7. Removals, unless there are suspiciously many.
    auto to_remove = count_entries(*db_, id, " AND seen_generation < ?", generation);
    if (!to_remove) return finish(RootSyncOutcome::Failed, to_remove.error().message);
    auto tx_final = db::Transaction::begin(*db_);
    if (!tx_final) return finish(RootSyncOutcome::Failed, tx_final.error().message);
    if (*to_remove > 0 && *prior_count > 0 &&
        *to_remove * 100 > static_cast<std::int64_t>(settings->mass_removal_threshold_percent) * *prior_count) {
        report.removals_held = true;
        report.held_removals = static_cast<std::uint64_t>(*to_remove);
    } else if (*to_remove > 0) {
        auto del = db_->prepare("DELETE FROM file_entries WHERE root_id = ? AND seen_generation < ?");
        if (!del) return finish(RootSyncOutcome::Failed, del.error().message);
        del->bind(1, id).bind(2, generation);
        if (auto r = del->run(); !r) return finish(RootSyncOutcome::Failed, r.error().message);
        report.removed = static_cast<std::uint64_t>(db_->changes());
    }

    // 8. Bookkeeping.
    auto old_errors = db_->prepare("DELETE FROM file_scan_errors WHERE root_id = ? AND generation < ?");
    if (!old_errors) return finish(RootSyncOutcome::Failed, old_errors.error().message);
    old_errors->bind(1, id).bind(2, generation);
    if (auto r = old_errors->run(); !r) return finish(RootSyncOutcome::Failed, r.error().message);

    auto update = db_->prepare("UPDATE file_roots SET generation = ?, status = ?, last_sync_ns = ?, held_removals = ?, "
                               "file_count = (SELECT COUNT(*) FROM file_entries WHERE root_id = ?) WHERE id = ?");
    if (!update) return finish(RootSyncOutcome::Failed, update.error().message);
    update->bind(1, generation)
        .bind(2, static_cast<std::int64_t>(RootStatus::Online))
        .bind(3, now)
        .bind(4, static_cast<std::int64_t>(report.held_removals))
        .bind(5, id)
        .bind(6, id);
    if (auto r = update->run(); !r) return finish(RootSyncOutcome::Failed, r.error().message);
    if (auto r = tx_final->commit(); !r) return finish(RootSyncOutcome::Failed, r.error().message);

    return finish(RootSyncOutcome::Synced, report.removals_held
                                               ? std::to_string(report.held_removals) +
                                                     " files are missing; confirm before they are removed"
                                               : std::string());
}

Result<std::uint64_t> Library::apply_held_removals(RootId id) {
    auto owner = root(id);
    if (!owner) return std::unexpected(owner.error());
    auto tx = db::Transaction::begin(*db_);
    if (!tx) return std::unexpected(database_error(tx.error()));
    auto del = db_->prepare("DELETE FROM file_entries WHERE root_id = ? AND seen_generation < ?");
    if (!del) return std::unexpected(database_error(del.error()));
    del->bind(1, id).bind(2, owner->generation);
    if (auto r = del->run(); !r) return std::unexpected(database_error(r.error()));
    const auto removed = static_cast<std::uint64_t>(db_->changes());
    auto update = db_->prepare("UPDATE file_roots SET held_removals = 0, "
                               "file_count = (SELECT COUNT(*) FROM file_entries WHERE root_id = ?) WHERE id = ?");
    if (!update) return std::unexpected(database_error(update.error()));
    update->bind(1, id).bind(2, id);
    if (auto r = update->run(); !r) return std::unexpected(database_error(r.error()));
    if (auto r = tx->commit(); !r) return std::unexpected(database_error(r.error()));
    return removed;
}

Result<std::vector<Entry>> Library::changed_entries(RootId root_id, std::int64_t generation, EntryId after,
                                                    std::size_t limit) {
    auto st = db_->prepare("SELECT " + std::string(detail::entry_columns()) +
                           " FROM file_entries WHERE root_id = ? AND changed_generation = ? AND id > ? "
                           "ORDER BY id LIMIT ?");
    if (!st) return std::unexpected(database_error(st.error()));
    st->bind(1, root_id).bind(2, generation).bind(3, after).bind(4, static_cast<std::int64_t>(limit));
    std::vector<Entry> out;
    while (true) {
        auto row = st->step();
        if (!row) return std::unexpected(database_error(row.error()));
        if (!*row) break;
        out.push_back(detail::read_entry(*st));
    }
    return out;
}

Result<std::vector<ScanErrorRecord>> Library::scan_errors(RootId root_id, std::size_t limit) {
    auto st = db_->prepare("SELECT root_id, relative_path, is_directory, message, generation FROM file_scan_errors "
                           "WHERE root_id = ? ORDER BY rowid LIMIT ?");
    if (!st) return std::unexpected(database_error(st.error()));
    st->bind(1, root_id).bind(2, static_cast<std::int64_t>(limit));
    std::vector<ScanErrorRecord> out;
    while (true) {
        auto row = st->step();
        if (!row) return std::unexpected(database_error(row.error()));
        if (!*row) break;
        out.push_back(ScanErrorRecord{st->column_int64(0), st->column_text(1), st->column_int64(2) != 0,
                                      st->column_text(3), st->column_int64(4)});
    }
    return out;
}

}
