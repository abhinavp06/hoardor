// The sync worker reads metadata after a sync (features/media_listing.md §4.1), on real files.

#include <hoardor/master/sync_worker.hpp>

#include "file/file_time.hpp"
#include "support/media_files.hpp"
#include "support/temp_dir.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <fstream>
#include <mutex>
#include <set>
#include <thread>

namespace fs = std::filesystem;
using namespace hoardor;

namespace {

constexpr std::int64_t old_mtime = 1'600'000'000'000'000'000;  // settled

class MetadataPassTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto database = db::Database::open(dir.path() / "library.db");
        ASSERT_TRUE(database.has_value());
        db = std::make_unique<db::Database>(std::move(*database));
        library = std::make_unique<file::Library>(std::move(*file::Library::open(*db, no_mounts())));
        tracks = std::make_unique<audio::Library>(std::move(*audio::Library::open(*db)));
        videos = std::make_unique<video::Library>(std::move(*video::Library::open(*db)));
    }

    static file::MountPointLister no_mounts() {
        return [] { return std::vector<fs::path>{}; };
    }

    file::CategoryId category(std::string_view name) {
        const auto all = library->categories().value();  // kept alive: see library_fixture.hpp
        for (const auto& c : all) {
            if (c.name == name) return c.id;
        }
        return 0;
    }

    void settle(const fs::path& folder) {
        for (const auto& e : fs::recursive_directory_iterator(folder)) {
            if (e.is_regular_file()) fs::last_write_time(e.path(), file::detail::from_unix_ns(old_mtime));
        }
    }

    struct Run {
        std::vector<master::MetadataProgress> progress;
        std::vector<master::MetadataReport> reports;
    };

    Run sync_all() {
        Run run;
        std::mutex m;
        auto worker = master::SyncWorker::start(
            dir.path() / "library.db",
            {.on_progress = {},
             .on_finished = {},
             .on_metadata_progress = [&](const master::MetadataProgress& p) { std::lock_guard l(m); run.progress.push_back(p); },
             .on_metadata_finished = [&](auto, const master::MetadataReport& r) { std::lock_guard l(m); run.reports.push_back(r); }},
            no_mounts());
        EXPECT_TRUE(worker.has_value());
        (*worker)->request_sync();
        (*worker)->wait_idle();
        return run;
    }

    test::TempDir dir;
    std::unique_ptr<db::Database> db;
    std::unique_ptr<file::Library> library;
    std::unique_ptr<audio::Library> tracks;
    std::unique_ptr<video::Library> videos;
};

}

TEST_F(MetadataPassTest, ReadsAudioAfterASyncAndOnlyChangesNextTime) {
    HOARDOR_SKIP_WITHOUT_FFMPEG();
    const fs::path music = dir.path() / "Music";
    for (int i = 1; i <= 3; ++i) {
        ASSERT_TRUE(test::make_media(music / "Album" / (std::to_string(i) + ".flac"),
                                     {.tags = {{"title", "Song " + std::to_string(i)}, {"album", "Album"},
                                               {"artist", "Band"}, {"track", std::to_string(i)}}}));
    }
    std::ofstream(music / "Album" / "broken.mp3") << "not audio at all";
    settle(music);
    ASSERT_TRUE(library->add_root(category("Music"), music, "Music", false));

    const Run first = sync_all();
    ASSERT_EQ(first.reports.size(), 1u);
    EXPECT_EQ(first.reports[0].read, 3u);
    EXPECT_EQ(first.reports[0].failed, 1u);
    EXPECT_FALSE(first.reports[0].cancelled);
    ASSERT_FALSE(first.progress.empty());
    EXPECT_EQ(first.progress.back().done, 4u);
    EXPECT_EQ(first.progress.back().total, 4u);

    const auto album = tracks->groups(std::vector<audio::Field>{audio::Field::AlbumArtist, audio::Field::Album}, {}).value().items;
    ASSERT_EQ(album.size(), 1u);
    EXPECT_EQ(album[0].values, (std::vector<std::string>{"Band", "Album"}));
    EXPECT_EQ(album[0].tracks, 3u);

    // Nothing changed: nothing to read.
    const Run second = sync_all();
    ASSERT_EQ(second.reports.size(), 1u);
    EXPECT_EQ(second.reports[0].read + second.reports[0].failed, 0u);

    // One file retagged: only it is read again.
    ASSERT_TRUE(test::make_media(music / "Album" / "2.flac", {.tags = {{"title", "Renamed"}, {"album", "Album"}, {"artist", "Band"}}}));
    settle(music);
    fs::last_write_time(music / "Album" / "2.flac", file::detail::from_unix_ns(old_mtime + 1'000'000'000));
    const Run third = sync_all();
    ASSERT_EQ(third.reports.size(), 1u);
    EXPECT_EQ(third.reports[0].read, 1u);
    EXPECT_EQ(tracks->count({{{audio::Field::Title, audio::Value{"renamed"}}}}).value(), 1u);
}

TEST_F(MetadataPassTest, UnsettledFilesWaitForALaterSync) {
    HOARDOR_SKIP_WITHOUT_FFMPEG();
    const fs::path music = dir.path() / "Music";
    ASSERT_TRUE(test::make_media(music / "new.flac", {}));  // just written: still "being copied"
    ASSERT_TRUE(library->add_root(category("Music"), music, "Music", false));
    const Run run = sync_all();
    EXPECT_TRUE(run.reports.empty() || run.reports[0].read == 0u);
    EXPECT_EQ(tracks->pending_count(std::nullopt).value(), 0u);  // not pending until it settles
}

TEST_F(MetadataPassTest, ReadsVideosWithTheirNfoAndPoster) {
    HOARDOR_SKIP_WITHOUT_FFMPEG();
    const fs::path movies = dir.path() / "Movies";
    const fs::path folder = movies / "Arrival (2016)";
    ASSERT_TRUE(test::make_video(folder / "Arrival (2016).mkv", "64x64", "-map 0:v -c:v libx264 -preset ultrafast -pix_fmt yuv420p"));
    std::ofstream(folder / "Arrival (2016).nfo") << "<movie><title>Arrival</title><year>2016</year><director>Denis Villeneuve</director></movie>";
    std::ofstream(folder / "poster.jpg") << "x";
    settle(movies);
    ASSERT_TRUE(library->add_root(category("Movies"), movies, "Movies", false));

    const Run run = sync_all();
    ASSERT_EQ(run.reports.size(), 1u);
    EXPECT_EQ(run.reports[0].read, 1u);
    const auto items = videos->items({}).value().items;
    ASSERT_EQ(items.size(), 1u);
    EXPECT_EQ(items[0].title, "Arrival");
    EXPECT_EQ(items[0].source, video::Source::Nfo);
    EXPECT_EQ(items[0].directors, (std::vector<std::string>{"Denis Villeneuve"}));
    EXPECT_NE(items[0].poster_entry, 0);
}

TEST_F(MetadataPassTest, EachMovieFindsItsPosterInAFlatFolder) {
    HOARDOR_SKIP_WITHOUT_FFMPEG();
    // Every movie in one folder: 300 other movies' sidecars sort before Zodiac's.
    const fs::path movies = dir.path() / "Movies";
    ASSERT_TRUE(test::make_video(movies / "Zodiac (2007).mkv", "64x64", "-map 0:v -c:v libx264 -preset ultrafast -pix_fmt yuv420p"));
    for (int i = 0; i < 150; ++i) {
        std::ofstream(movies / ("Movie " + std::to_string(i) + " (2000).nfo")) << "<movie></movie>";
        std::ofstream(movies / ("Movie " + std::to_string(i) + " (2000)-poster.jpg")) << "x";
    }
    std::ofstream(movies / "Zodiac (2007).nfo") << "<movie><title>Zodiac</title><year>2007</year></movie>";
    std::ofstream(movies / "Zodiac (2007)-poster.jpg") << "x";
    settle(movies);
    ASSERT_TRUE(library->add_root(category("Movies"), movies, "Movies", false));

    sync_all();
    const auto items = videos->items({}).value().items;
    ASSERT_EQ(items.size(), 1u);
    EXPECT_EQ(items[0].source, video::Source::Nfo);
    ASSERT_NE(items[0].poster_entry, 0);
    EXPECT_EQ(library->entry(items[0].poster_entry)->relative_path, "Zodiac (2007)-poster.jpg");
}

TEST_F(MetadataPassTest, CancelStopsThePassAndTheRestWaits) {
    HOARDOR_SKIP_WITHOUT_FFMPEG();
    const fs::path music = dir.path() / "Music";
    ASSERT_TRUE(test::make_media(music / "a.flac", {}));
    for (int i = 0; i < 300; ++i) fs::copy_file(music / "a.flac", music / ("c" + std::to_string(i) + ".flac"));
    settle(music);
    ASSERT_TRUE(library->add_root(category("Music"), music, "Music", false));

    std::mutex m;
    std::vector<master::MetadataReport> reports;
    std::unique_ptr<master::SyncWorker> worker;
    auto started = master::SyncWorker::start(
        dir.path() / "library.db",
        {.on_progress = {},
         .on_finished = {},
         .on_metadata_progress = [&](const master::MetadataProgress& p) {
             if (p.done > 0 && p.done < p.total) worker->cancel();  // as soon as reading is under way
         },
         .on_metadata_finished = [&](auto, const master::MetadataReport& r) { std::lock_guard l(m); reports.push_back(r); }},
        no_mounts());
    ASSERT_TRUE(started.has_value());
    worker = std::move(*started);
    worker->request_sync();
    worker->wait_idle();
    std::lock_guard l(m);
    ASSERT_EQ(reports.size(), 1u);
    if (reports[0].cancelled) {  // a fast machine may finish before the first progress report
        EXPECT_LT(reports[0].read, 301u);
        EXPECT_GT(tracks->pending_count(std::nullopt).value(), 0u);
    }
    EXPECT_EQ(tracks->count({}).value() + tracks->pending_count(std::nullopt).value(), 301u);
}

TEST_F(MetadataPassTest, ASyncRequestPausesThePassWhichThenResumes) {
    HOARDOR_SKIP_WITHOUT_FFMPEG();
    const fs::path music = dir.path() / "Music";
    ASSERT_TRUE(test::make_media(music / "a.flac", {}));
    for (int i = 0; i < 400; ++i) fs::copy_file(music / "a.flac", music / ("c" + std::to_string(i) + ".flac"));
    settle(music);
    ASSERT_TRUE(library->add_root(category("Music"), music, "Music", false));

    std::mutex m;
    std::vector<master::MetadataReport> reports;
    std::atomic<int> syncs{0};
    std::atomic<bool> asked{false};
    std::unique_ptr<master::SyncWorker> worker;
    auto started = master::SyncWorker::start(
        dir.path() / "library.db",
        {.on_progress = {},
         .on_finished = [&](auto, const file::SyncReport&) { ++syncs; },
         .on_metadata_progress = [&](const master::MetadataProgress& p) {
             // Mid-pass, ask for another sync: it runs first, then the pass continues.
             if (p.done > 0 && p.done < p.total && !asked.exchange(true)) {
                 EXPECT_TRUE(worker->request_sync());
                 EXPECT_TRUE(worker->syncing());
                 EXPECT_TRUE(worker->reading());
             }
         },
         .on_metadata_finished = [&](auto, const master::MetadataReport& r) { std::lock_guard l(m); reports.push_back(r); }},
        no_mounts());
    ASSERT_TRUE(started.has_value());
    worker = std::move(*started);
    worker->request_sync();
    worker->wait_idle();
    EXPECT_FALSE(worker->syncing());
    EXPECT_FALSE(worker->reading());
    std::lock_guard l(m);
    ASSERT_EQ(reports.size(), 1u);  // the paused pass reports once, when it's really done
    EXPECT_FALSE(reports[0].cancelled);
    EXPECT_GT(reports[0].elapsed_ms, 0);
    EXPECT_EQ(tracks->count({}).value(), 401u);
    EXPECT_EQ(syncs.load(), asked ? 2 : 1);  // a fast machine may finish before the first progress report
    RecordProperty("paused", asked ? "yes" : "no");
}

TEST_F(MetadataPassTest, TwoDrivesSyncAndReadInParallel) {
    HOARDOR_SKIP_WITHOUT_FFMPEG();
    // Two folders on separate (pretend) drives, 150 tracks each.
    for (const char* drive : {"DriveA", "DriveB"}) {
        const fs::path music = dir.path() / drive / "Music";
        ASSERT_TRUE(test::make_media(music / "a.flac", {.tags = {{"album", drive}, {"artist", "X"}}}));
        for (int i = 0; i < 149; ++i) fs::copy_file(music / "a.flac", music / ("c" + std::to_string(i) + ".flac"));
        settle(music);
        ASSERT_TRUE(library->add_root(category("Music"), music, drive, false));
    }
    std::mutex m;
    std::set<std::thread::id> sync_threads;
    std::vector<file::SyncReport> syncs;
    std::vector<master::MetadataReport> passes;
    auto worker = master::SyncWorker::start(
        dir.path() / "library.db",
        {.on_progress = [&](const file::SyncProgress& p) {
             std::lock_guard l(m);
             sync_threads.insert(std::this_thread::get_id());
             EXPECT_EQ(p.root_count, 2u);
         },
         .on_finished = [&](auto, const file::SyncReport& r) { std::lock_guard l(m); syncs.push_back(r); },
         .on_metadata_progress = {},
         .on_metadata_finished = [&](auto, const master::MetadataReport& r) { std::lock_guard l(m); passes.push_back(r); }},
        no_mounts(), [](const fs::path& p) { return p.string().find("DriveA") != std::string::npos ? "a" : "b"; });
    ASSERT_TRUE(worker.has_value());
    (*worker)->request_sync();
    (*worker)->wait_idle();

    std::lock_guard l(m);
    ASSERT_EQ(syncs.size(), 1u);
    ASSERT_EQ(syncs[0].roots.size(), 2u);
    for (const auto& r : syncs[0].roots) {
        EXPECT_EQ(r.outcome, file::RootSyncOutcome::Synced) << r.message;
        EXPECT_EQ(r.added, 150u);
    }
    EXPECT_LT(syncs[0].roots[0].root_id, syncs[0].roots[1].root_id);  // reports keep the roots' order
    EXPECT_GE(sync_threads.size(), 2u);                                 // two drives, two workers
    ASSERT_EQ(passes.size(), 1u);
    EXPECT_EQ(passes[0].read, 300u);
    EXPECT_EQ(tracks->group_count(std::vector<audio::Field>{audio::Field::Album}, {}).value(), 2u);
}

TEST_F(MetadataPassTest, OneDriveAtATimeWhenTheSettingSaysSo) {
    HOARDOR_SKIP_WITHOUT_FFMPEG();
    for (const char* drive : {"DriveA", "DriveB"}) {
        const fs::path music = dir.path() / drive / "Music";
        ASSERT_TRUE(test::make_media(music / "a.flac", {}));
        settle(music);
        ASSERT_TRUE(library->add_root(category("Music"), music, drive, false));
    }
    auto s = library->load_settings().value();
    s.parallel_devices = 1;
    ASSERT_TRUE(library->save_settings(s));
    std::mutex m;
    std::set<std::thread::id> threads;
    auto worker = master::SyncWorker::start(
        dir.path() / "library.db",
        {.on_progress = [&](const file::SyncProgress&) { std::lock_guard l(m); threads.insert(std::this_thread::get_id()); },
         .on_finished = {}, .on_metadata_progress = {}, .on_metadata_finished = {}},
        no_mounts(), [](const fs::path& p) { return p.string().find("DriveA") != std::string::npos ? "a" : "b"; });
    ASSERT_TRUE(worker.has_value());
    (*worker)->request_sync();
    (*worker)->wait_idle();
    EXPECT_EQ(threads.size(), 1u);
    EXPECT_EQ(tracks->count({}).value(), 2u);
}
