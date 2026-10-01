#include <hoardor/master/sync_worker.hpp>

#include "support/temp_dir.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <latch>

namespace fs = std::filesystem;
using namespace hoardor;
using hoardor::test::TempDir;

namespace {

// A database file with a Music folder of `files` tracks, set up on the test's own connection.
class SyncWorkerTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto database = db::Database::open(db_file());
        ASSERT_TRUE(database.has_value());
        db = std::make_unique<db::Database>(std::move(*database));
        auto lib = file::Library::open(*db, [] { return std::vector<fs::path>{}; });
        ASSERT_TRUE(lib.has_value());
        library = std::make_unique<file::Library>(std::move(*lib));
    }

    fs::path db_file() const { return dir.path() / "library.db"; }

    file::CategoryId music() {
        const auto all = library->categories().value();
        return all.front().id;  // Music is seeded first
    }

    void add_music_root(int files) {
        for (int i = 0; i < files; ++i) dir.write("Music/t" + std::to_string(i) + ".mp3");
        ASSERT_TRUE(library->add_root(music(), dir.path() / "Music"));
    }

    void set_settings(const std::function<void(file::Settings&)>& edit) {
        auto s = library->load_settings().value();
        edit(s);
        ASSERT_TRUE(library->save_settings(s));
    }

    TempDir dir;
    std::unique_ptr<db::Database> db;
    std::unique_ptr<file::Library> library;
};

auto no_mounts() {
    return [] { return std::vector<fs::path>{}; };
}

}

TEST_F(SyncWorkerTest, RunsARequestedSyncInTheBackground) {
    add_music_root(5);
    std::mutex m;
    std::vector<file::SyncReport> reports;
    auto worker = master::SyncWorker::start(
        db_file(),
        {.on_progress = {},
         .on_finished = [&](auto, const file::SyncReport& r) {
             std::lock_guard lock(m);
             reports.push_back(r);
         }},
        no_mounts());
    ASSERT_TRUE(worker.has_value()) << worker.error().message;
    EXPECT_TRUE((*worker)->idle());
    EXPECT_TRUE((*worker)->request_sync());
    (*worker)->wait_idle();
    std::lock_guard lock(m);
    ASSERT_EQ(reports.size(), 1u);
    ASSERT_EQ(reports[0].roots.size(), 1u);
    EXPECT_EQ(reports[0].roots[0].outcome, file::RootSyncOutcome::Synced);
    EXPECT_EQ(reports[0].roots[0].added, 5u);
    EXPECT_EQ(library->entries(reports[0].roots[0].root_id).value().size(), 5u) << "visible on another connection";
}

TEST_F(SyncWorkerTest, DuplicateRequestsAreIgnored) {
    add_music_root(3);
    std::latch entered(1);
    std::promise<void> release;
    auto released = release.get_future().share();
    std::atomic<int> progress_calls{0};
    std::atomic<int> finished{0};
    auto worker = master::SyncWorker::start(
        db_file(),
        {.on_progress =
             [&](const file::SyncProgress&) {
                 if (progress_calls++ == 0) {
                     entered.count_down();
                     released.wait();  // hold the first sync open
                 }
             },
         .on_finished = [&](auto, const auto&) { ++finished; }},
        no_mounts());
    ASSERT_TRUE(worker.has_value());
    auto& w = **worker;
    ASSERT_TRUE(w.request_sync(music()));
    entered.wait();
    EXPECT_FALSE(w.request_sync(music())) << "the same scope is running";
    EXPECT_TRUE(w.request_sync()) << "a global sync covers more, so it's queued";
    EXPECT_FALSE(w.request_sync()) << "the same scope is already queued";
    EXPECT_FALSE(w.idle());
    release.set_value();
    w.wait_idle();
    EXPECT_EQ(finished.load(), 2);
}

TEST_F(SyncWorkerTest, CancelStopsTheRunningSyncAndRemovesNothing) {
    add_music_root(20);
    set_settings([](file::Settings& s) { s.progress_interval_files = 1; });
    std::latch entered(1);
    std::promise<void> release;
    auto released = release.get_future().share();
    std::atomic<int> calls{0};
    std::optional<file::SyncReport> report;
    std::mutex m;
    auto worker = master::SyncWorker::start(
        db_file(),
        {.on_progress =
             [&](const file::SyncProgress&) {
                 if (calls++ == 2) {
                     entered.count_down();
                     released.wait();
                 }
             },
         .on_finished =
             [&](auto, const file::SyncReport& r) {
                 std::lock_guard lock(m);
                 report = r;
             }},
        no_mounts());
    ASSERT_TRUE(worker.has_value());
    ASSERT_TRUE((*worker)->request_sync());
    entered.wait();
    (*worker)->cancel();
    release.set_value();
    (*worker)->wait_idle();
    std::lock_guard lock(m);
    ASSERT_TRUE(report.has_value());
    EXPECT_TRUE(report->cancelled);
    EXPECT_EQ(report->roots.at(0).outcome, file::RootSyncOutcome::Cancelled);
    EXPECT_EQ(report->roots.at(0).removed, 0u);
}

TEST_F(SyncWorkerTest, SyncOnStartupWhenEnabled) {
    add_music_root(2);
    set_settings([](file::Settings& s) { s.sync_on_startup = true; });
    std::atomic<int> finished{0};
    std::optional<file::CategoryId> scope{-1};
    auto worker = master::SyncWorker::start(
        db_file(), {.on_progress = {}, .on_finished = [&](auto s, const auto&) { scope = s; ++finished; }}, no_mounts());
    ASSERT_TRUE(worker.has_value());
    (*worker)->wait_idle();
    EXPECT_EQ(finished.load(), 1);
    EXPECT_EQ(scope, std::nullopt) << "a global sync";
}

TEST_F(SyncWorkerTest, NoStartupSyncByDefault) {
    add_music_root(2);
    std::atomic<int> finished{0};
    auto worker = master::SyncWorker::start(db_file(), {.on_progress = {}, .on_finished = [&](auto, const auto&) { ++finished; }},
                                            no_mounts());
    ASSERT_TRUE(worker.has_value());
    EXPECT_TRUE((*worker)->idle());
    (*worker)->wait_idle();
    EXPECT_EQ(finished.load(), 0);
}

TEST_F(SyncWorkerTest, OtherConnectionsCanReadAndWriteDuringASync) {
    add_music_root(10);
    set_settings([](file::Settings& s) { s.progress_interval_files = 2; });
    std::latch entered(1);
    std::promise<void> release;
    auto released = release.get_future().share();
    std::atomic<int> calls{0};
    auto worker = master::SyncWorker::start(
        db_file(),
        {.on_progress =
             [&](const file::SyncProgress&) {
                 if (calls++ == 2) {
                     entered.count_down();
                     released.wait();
                 }
             },
         .on_finished = {}},
        no_mounts());
    ASSERT_TRUE(worker.has_value());
    ASSERT_TRUE((*worker)->request_sync());
    entered.wait();
    // Mid-sync: the UI's connection reads, and even writes (e.g. a setting), without waiting.
    const auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(library->roots().value().size(), 1u);
    EXPECT_FALSE(library->entries(library->roots().value()[0].id).value().empty()) << "committed batches are visible";
    EXPECT_TRUE(library->add_category("Podcasts", {file::FileKind::Audio}));
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(1));
    release.set_value();
    (*worker)->wait_idle();
}

TEST_F(SyncWorkerTest, DestructorStopsARunningSyncPromptly) {
    add_music_root(200);
    set_settings([](file::Settings& s) { s.progress_interval_files = 1; });
    std::latch entered(1);
    std::atomic<int> calls{0};
    auto worker = master::SyncWorker::start(
        db_file(),
        {.on_progress =
             [&](const file::SyncProgress&) {
                 if (calls++ == 1) entered.count_down();
                 std::this_thread::sleep_for(std::chrono::milliseconds(5));
             },
         .on_finished = {}},
        no_mounts());
    ASSERT_TRUE(worker.has_value());
    ASSERT_TRUE((*worker)->request_sync());
    entered.wait();
    const auto start = std::chrono::steady_clock::now();
    worker->reset();  // closing the app mid-sync
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::milliseconds(500));
    EXPECT_LT(calls.load(), 200) << "it stopped instead of finishing";
}

TEST_F(SyncWorkerTest, StartFailsWhenTheDatabaseCannotBeOpened) {
    auto worker = master::SyncWorker::start(dir.path() / "missing" / "x.db", {}, no_mounts());
    ASSERT_FALSE(worker.has_value());
    EXPECT_EQ(worker.error().code, file::ErrorCode::Database);
}
