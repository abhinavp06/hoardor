// player::Player on libmpv, headless (ao=null, vo=null), with files the ffmpeg CLI makes
// (features/player.md §8). The null audio output plays in real time, so files stay short.

#include <hoardor/file/library.hpp>
#include <hoardor/master/playback.hpp>
#include <hoardor/player/player.hpp>

#include "support/media_files.hpp"
#include "support/temp_dir.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace fs = std::filesystem;
using namespace hoardor;
using player::EntryId;
using player::State;

namespace {

template <class F>
bool eventually(F condition, int timeout_ms = 8000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < end) {
        if (condition()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return condition();
}

// What the callbacks saw, from the player's thread.
struct Recorder {
    std::mutex m;
    std::vector<EntryId> started;                       // each entry once it plays (consecutive repeats folded)
    std::vector<std::pair<EntryId, std::string>> errors;
    player::Status last;
    int queue_changes = 0;

    player::Callbacks callbacks() {
        return {[this](const player::Status& s) {
                    std::lock_guard lock(m);
                    last = s;
                    if (s.state == State::Playing && s.entry && (started.empty() || started.back() != *s.entry)) {
                        started.push_back(*s.entry);
                    }
                },
                nullptr,
                [this] {
                    std::lock_guard lock(m);
                    ++queue_changes;
                },
                [this](EntryId e, const player::Error& error) {
                    std::lock_guard lock(m);
                    errors.emplace_back(e, error.message);
                }};
    }
    std::vector<EntryId> started_copy() {
        std::lock_guard lock(m);
        return started;
    }
    std::vector<std::pair<EntryId, std::string>> errors_copy() {
        std::lock_guard lock(m);
        return errors;
    }
    player::Status status() {
        std::lock_guard lock(m);
        return last;
    }
};

class PlayerTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (test::ffmpeg_tool().empty()) GTEST_SKIP() << "the ffmpeg command-line tool isn't installed";
        auto database = db::Database::open(database_file());
        ASSERT_TRUE(database.has_value());
        db.emplace(std::move(*database));
        auto files = file::Library::open(*db);
        ASSERT_TRUE(files.has_value());
        library.emplace(std::move(*files));
    }

    fs::path database_file() const { return dir.path() / "library.db"; }

    // An audio file of `seconds` (a tone, so mpv has something to decode).
    void tone(const std::string& name, double seconds) {
        test::MediaSpec spec;
        spec.input = "sine=frequency=440:sample_rate=44100";
        spec.seconds = seconds;
        ASSERT_TRUE(test::make_media(dir.path() / "music" / name, spec));
    }
    // A video of `seconds` with a silent audio track.
    void clip(const std::string& name, int seconds) {
        fs::create_directories(dir.path() / "music");
        ASSERT_TRUE(test::run_ffmpeg("-f lavfi -t " + std::to_string(seconds) + " -i testsrc=size=64x48:rate=10 -f lavfi -t " +
                                     std::to_string(seconds) + " -i anullsrc=r=48000:cl=stereo -c:v libx264 -preset ultrafast "
                                     "-pix_fmt yuv420p -c:a aac " + test::quoted((dir.path() / "music" / name).string())));
    }

    // Adds the music folder as a root and syncs it: every file gets an entry id.
    void sync() {
        if (!root) {
            const auto category = library->add_category("Everything", {file::FileKind::Audio, file::FileKind::Video});
            ASSERT_TRUE(category.has_value());
            root = library->add_root(*category, dir.path() / "music", "Music", false)->id;
        }
        ASSERT_EQ(library->sync_root(root).outcome, file::RootSyncOutcome::Synced);
        for (file::EntryId after = 0;;) {
            const auto page = library->entries(root, after, 100).value();
            if (page.empty()) break;
            for (const auto& e : page) {
                ids[e.relative_path] = e.id;
                paths[e.id] = dir.path() / "music" / fs::path(e.relative_path);
                after = e.id;
            }
        }
    }

    // Resolves from the test's own map; entries listed in `failing` fail as "offline".
    player::Resolver resolver() {
        return [this](EntryId e) -> player::Result<fs::path> {
            {
                std::lock_guard lock(resolver_mutex);
                ++resolved;
                if (failing.contains(e)) return std::unexpected(player::Error{"its drive is offline"});
            }
            if (resolve_delay_ms) std::this_thread::sleep_for(std::chrono::milliseconds(resolve_delay_ms));
            const auto it = paths.find(e);
            if (it == paths.end()) return std::unexpected(player::Error{"no such entry"});
            return it->second;
        };
    }

    std::unique_ptr<player::Player> start(Recorder& recorder) {
        auto p = player::Player::start(database_file(), resolver(), recorder.callbacks(), {"null", "null"});
        EXPECT_TRUE(p.has_value()) << (p ? "" : p.error().message);
        return p ? std::move(*p) : nullptr;
    }

    player::ItemState state(EntryId e) {
        auto items = player::Library::open(*db);
        return items->state(e).value();
    }

    test::TempDir dir;
    std::optional<db::Database> db;
    std::optional<file::Library> library;
    file::RootId root = 0;
    std::map<std::string, EntryId> ids;
    std::map<EntryId, fs::path> paths;
    std::mutex resolver_mutex;
    std::set<EntryId> failing;
    int resolved = 0;
    int resolve_delay_ms = 0;
};

}

TEST_F(PlayerTest, PlaysTheQueueInOrderAndCountsEachPlay) {
    tone("a.flac", 0.6);
    tone("b.flac", 0.6);
    tone("c.flac", 0.6);
    sync();
    Recorder r;
    auto p = start(r);
    ASSERT_TRUE(p);
    const std::vector<EntryId> album{ids["a.flac"], ids["b.flac"], ids["c.flac"]};
    p->play_now(album);
    ASSERT_TRUE(eventually([&] { return r.started_copy().size() == 3 && r.status().state == State::Idle; }));
    EXPECT_EQ(r.started_copy(), album);
    EXPECT_EQ(p->queue(), album);   // a finished queue stays until it's cleared
    for (EntryId e : album) {
        const auto st = state(e);
        EXPECT_EQ(st.play_count, 1) << e << " position " << st.position_ms << " duration " << st.duration_ms << " last " << st.last_played_ns;
    }
    EXPECT_TRUE(r.errors_copy().empty());
}

TEST_F(PlayerTest, PromptActionsAddAndClearAndPlay) {
    tone("a.flac", 5);
    tone("b.flac", 5);
    tone("c.flac", 5);
    sync();
    Recorder r;
    auto p = start(r);
    p->play_now({ids["a.flac"], ids["b.flac"]});
    ASSERT_TRUE(eventually([&] { return r.status().state == State::Playing; }));
    p->add({ids["c.flac"]});                                   // "add to queue"
    ASSERT_TRUE(eventually([&] { return p->queue().size() == 3; }));
    EXPECT_EQ(r.status().entry, ids["a.flac"]);                 // still playing the first
    p->play_now({ids["c.flac"]});                               // "clear queue and play"
    ASSERT_TRUE(eventually([&] { return r.status().entry == ids["c.flac"] && r.status().state == State::Playing; }));
    EXPECT_EQ(p->queue(), (std::vector<EntryId>{ids["c.flac"]}));
    p->clear();
    ASSERT_TRUE(eventually([&] { return r.status().state == State::Idle && p->queue().empty(); }));
}

TEST_F(PlayerTest, AnOfflineFileIsReportedOnceAndSkipped) {
    tone("a.flac", 0.5);
    tone("b.flac", 0.5);
    tone("c.flac", 0.5);
    sync();
    failing.insert(ids["b.flac"]);
    Recorder r;
    auto p = start(r);
    p->play_now({ids["a.flac"], ids["b.flac"], ids["c.flac"]});
    ASSERT_TRUE(eventually([&] { return r.started_copy().size() == 2 && r.status().state == State::Idle; }));
    EXPECT_EQ(r.started_copy(), (std::vector<EntryId>{ids["a.flac"], ids["c.flac"]}));
    const auto errors = r.errors_copy();
    ASSERT_EQ(errors.size(), 1u);
    EXPECT_EQ(errors[0].first, ids["b.flac"]);
    EXPECT_EQ(errors[0].second, "its drive is offline");
}

TEST_F(PlayerTest, NothingPlayableStopsInsteadOfLooping) {
    tone("a.flac", 0.5);
    tone("b.flac", 0.5);
    sync();
    failing = {ids["a.flac"], ids["b.flac"]};
    Recorder r;
    auto p = start(r);
    p->play_now({ids["a.flac"], ids["b.flac"]});
    ASSERT_TRUE(eventually([&] { return r.errors_copy().size() == 2; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    EXPECT_EQ(r.errors_copy().size(), 2u);   // one pass, no loop
    EXPECT_EQ(p->status().state, State::Idle);
    EXPECT_TRUE(r.started_copy().empty());
}

TEST_F(PlayerTest, ACorruptFileIsSkipped) {
    tone("a.flac", 0.5);
    fs::create_directories(dir.path() / "music");
    std::ofstream(dir.path() / "music" / "broken.flac") << "not audio at all";
    tone("c.flac", 0.5);
    sync();
    Recorder r;
    auto p = start(r);
    p->play_now({ids["a.flac"], ids["broken.flac"], ids["c.flac"]});
    ASSERT_TRUE(eventually([&] { return r.started_copy().size() == 2 && r.status().state == State::Idle; }));
    EXPECT_EQ(r.started_copy(), (std::vector<EntryId>{ids["a.flac"], ids["c.flac"]}));
    const auto errors = r.errors_copy();
    ASSERT_EQ(errors.size(), 1u);
    EXPECT_EQ(errors[0].first, ids["broken.flac"]);
}

TEST_F(PlayerTest, PauseSeekPreviousAndNext) {
    tone("a.flac", 8);
    tone("b.flac", 8);
    sync();
    Recorder r;
    auto p = start(r);
    p->play_now({ids["a.flac"], ids["b.flac"]});
    ASSERT_TRUE(eventually([&] { return r.status().state == State::Playing; }));
    p->pause();
    ASSERT_TRUE(eventually([&] { return r.status().state == State::Paused; }));
    p->seek(4000);
    ASSERT_TRUE(eventually([&] { return std::abs(p->status().position_ms - 4000) < 300; }));
    p->previous();   // past 3 s: back to the start of this track
    ASSERT_TRUE(eventually([&] { return p->status().position_ms < 300; }));
    EXPECT_EQ(p->status().entry, ids["a.flac"]);
    p->previous();   // at the start of the first track: stays
    p->next();
    ASSERT_TRUE(eventually([&] { return r.status().entry == ids["b.flac"] && r.status().state == State::Playing; }));
    p->previous();   // early in b: back to a
    ASSERT_TRUE(eventually([&] { return r.status().entry == ids["a.flac"]; }));
    p->seek(999'999);   // clamped to the length
    ASSERT_TRUE(eventually([&] { return r.status().entry == ids["b.flac"] || r.status().state == State::Idle; }));
}

TEST_F(PlayerTest, VideoResumesUntilViewed) {
    clip("film.mp4", 4);
    sync();
    {   // low thresholds, so a 4 s clip can show the rules
        auto items = player::Library::open(*db);
        player::Settings s = player::Settings::defaults();
        s.resume_min_seconds = 1;
        s.viewed_percent = 50;
        ASSERT_TRUE(items->save_settings(s));
    }
    const EntryId film = ids["film.mp4"];
    {
        Recorder r;
        auto p = start(r);
        p->play_now({film});
        ASSERT_TRUE(eventually([&] { return r.status().state == State::Playing && r.status().has_video; }));
        p->pause();
        p->seek(1500);
        ASSERT_TRUE(eventually([&] { return state(film).position_ms >= 1400; }));
        EXPECT_FALSE(state(film).viewed);
    }   // closing saves too
    {
        Recorder r;
        auto p = start(r);
        p->play_now({film});
        ASSERT_TRUE(eventually([&] { return r.status().state == State::Playing; }));
        EXPECT_NEAR(static_cast<double>(r.status().resumed_from_ms), 1500.0, 150.0);
        ASSERT_TRUE(eventually([&] { return r.status().state == State::Idle; }));   // plays to the end
        EXPECT_TRUE(state(film).viewed);
    }
    {
        Recorder r;
        auto p = start(r);
        p->play_now({film});
        ASSERT_TRUE(eventually([&] { return r.status().state == State::Playing; }));
        EXPECT_EQ(r.status().resumed_from_ms, 0);   // viewed: from the start
    }
}

TEST_F(PlayerTest, MusicStartsAtTheBeginningByDefault) {
    tone("long.flac", 6);
    sync();
    ASSERT_TRUE(player::Library::open(*db)->save_position(ids["long.flac"], 4000, 6000, false, 1));
    Recorder r;
    auto p = start(r);
    p->play_now({ids["long.flac"]});
    ASSERT_TRUE(eventually([&] { return r.status().state == State::Playing; }));
    EXPECT_EQ(r.status().resumed_from_ms, 0);
    EXPECT_FALSE(r.status().has_video);
}

TEST_F(PlayerTest, VolumeAndMuteAreRemembered) {
    {
        Recorder r;
        auto p = start(r);
        p->set_volume(40);
        p->set_muted(true);
        ASSERT_TRUE(eventually([&] { return r.status().volume == 40 && r.status().muted; }));
    }
    Recorder r;
    auto p = start(r);
    EXPECT_EQ(p->status().volume, 40);
    EXPECT_TRUE(p->status().muted);
}

TEST_F(PlayerTest, CommandsReturnAtOnceWhileADriveSpinsUp) {
    tone("a.flac", 2);
    sync();
    resolve_delay_ms = 1500;
    Recorder r;
    auto p = start(r);
    const auto before = std::chrono::steady_clock::now();
    p->play_now({ids["a.flac"]});
    p->set_volume(70);
    EXPECT_LT(std::chrono::steady_clock::now() - before, std::chrono::milliseconds(100));
    ASSERT_TRUE(eventually([&] { return r.status().state == State::Playing; }));
}

TEST_F(PlayerTest, RemovingTheNextOrTheCurrentItem) {
    tone("a.flac", 5);
    tone("b.flac", 5);
    tone("c.flac", 5);
    sync();
    Recorder r;
    auto p = start(r);
    p->play_now({ids["a.flac"], ids["b.flac"], ids["c.flac"]});
    ASSERT_TRUE(eventually([&] { return r.status().state == State::Playing; }));
    p->remove(1);   // b, which mpv already held as its next file
    ASSERT_TRUE(eventually([&] { return p->queue() == std::vector<EntryId>{ids["a.flac"], ids["c.flac"]}; }));
    p->remove(0);   // the playing one: c plays
    ASSERT_TRUE(eventually([&] { return r.status().entry == ids["c.flac"] && r.status().state == State::Playing; }));
    EXPECT_EQ(p->current(), 0u);
    p->jump(5);     // out of range: nothing
    p->remove(7);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(r.status().entry, ids["c.flac"]);
}

TEST_F(PlayerTest, MpvWritesItsLogWhenAsked) {
    tone("a.flac", 0.5);
    sync();
    const fs::path log = dir.path() / "mpv.log";
    {
        Recorder r;
        auto p = player::Player::start(database_file(), resolver(), r.callbacks(), {"null", "null", log});
        ASSERT_TRUE(p.has_value()) << p.error().message;
        (*p)->play_now({ids["a.flac"]});
        ASSERT_TRUE(eventually([&] { return r.status().state == State::Playing; }));
    }
    std::ifstream in(log);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_NE(text.find("a.flac"), std::string::npos);   // what it opened, and how
}

TEST_F(PlayerTest, MastersResolverSaysOfflineAndMissing) {
    tone("a.flac", 0.5);
    tone("b.flac", 0.5);
    sync();
    const auto resolve = master::file_resolver(database_file());
    ASSERT_TRUE(resolve(ids["a.flac"]).has_value());
    EXPECT_EQ(resolve(ids["a.flac"]).value(), paths[ids["a.flac"]]);
    fs::remove(dir.path() / "music" / "b.flac");
    EXPECT_EQ(resolve(ids["b.flac"]).error().message, "the file is missing");
    EXPECT_EQ(resolve(424242).error().message, "it's no longer in the library");
    fs::rename(dir.path() / "music", dir.path() / "unplugged");
    EXPECT_EQ(resolve(ids["a.flac"]).error().message, "its drive is offline");
}
