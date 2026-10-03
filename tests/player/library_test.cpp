// player::Library: player_items and player_settings on real entries (features/player.md §5–§7).

#include <hoardor/player/player.hpp>

#include "support/library_fixture.hpp"

#include <gtest/gtest.h>

namespace fs = std::filesystem;
using namespace hoardor;

namespace {

class PlayerLibraryTest : public test::LibraryTest {
protected:
    void SetUp() override {
        LibraryTest::SetUp();
        for (const char* name : {"a.flac", "b.flac", "c.flac"}) dir.write(fs::path("music") / name, 1);
        root = library->add_root(category("Music"), dir.path() / "music", "Music", false)->id;
        ASSERT_EQ(library->sync_root(root).outcome, file::RootSyncOutcome::Synced);
        for (const auto& e : all_entries(root)) ids.push_back(e.id);
        auto lib = player::Library::open(*db);
        ASSERT_TRUE(lib.has_value()) << lib.error().message;
        items = std::make_unique<player::Library>(std::move(*lib));
    }

    file::RootId root = 0;
    std::vector<player::EntryId> ids;
    std::unique_ptr<player::Library> items;
};

}

TEST_F(PlayerLibraryTest, StatesAreZerosUntilPlayedOrLikedInTheAskedOrder) {
    ASSERT_TRUE(items->count_play(ids[1], 5));
    const auto states = items->states(std::vector<player::EntryId>{ids[2], ids[1], ids[0]}).value();
    ASSERT_EQ(states.size(), 3u);
    EXPECT_EQ(states[0].entry, ids[2]);
    EXPECT_EQ(states[0].play_count, 0);
    EXPECT_EQ(states[1].entry, ids[1]);
    EXPECT_EQ(states[1].play_count, 1);
    EXPECT_EQ(states[1].last_played_ns, 5);
    EXPECT_TRUE(items->states({}).value().empty());
    EXPECT_EQ(items->state(ids[0]).value().entry, ids[0]);   // no row: zeros, not an error
}

TEST_F(PlayerLibraryTest, LikeUnlikeLikeAgain) {
    ASSERT_TRUE(items->set_liked(ids[0], true, 100));
    EXPECT_TRUE(items->state(ids[0])->liked());
    EXPECT_EQ(items->state(ids[0])->liked_ns, 100);
    ASSERT_TRUE(items->set_liked(ids[0], false, 200));
    EXPECT_FALSE(items->state(ids[0])->liked());
    ASSERT_TRUE(items->set_liked(ids[0], true, 300));
    EXPECT_EQ(items->state(ids[0])->liked_ns, 300);
    // Liking keeps what playing stored, and the other way round.
    ASSERT_TRUE(items->save_position(ids[0], 1000, 9000, false, 400));
    ASSERT_TRUE(items->set_liked(ids[0], true, 500));
    EXPECT_EQ(items->state(ids[0])->position_ms, 1000);
    EXPECT_FALSE(items->set_liked(9999, true, 1).has_value());   // no such entry (foreign key)
}

TEST_F(PlayerLibraryTest, ViewedOnlyEverTurnsOn) {
    ASSERT_TRUE(items->save_position(ids[0], 8500, 9000, true, 1));
    ASSERT_TRUE(items->save_position(ids[0], 300, 9000, false, 2));   // watching it again
    const auto s = items->state(ids[0]).value();
    EXPECT_TRUE(s.viewed);
    EXPECT_EQ(s.position_ms, 300);
    EXPECT_EQ(s.duration_ms, 9000);
    EXPECT_EQ(s.last_played_ns, 2);
}

TEST_F(PlayerLibraryTest, PlaysAreCounted) {
    for (int i = 0; i < 3; ++i) ASSERT_TRUE(items->count_play(ids[2], i));
    EXPECT_EQ(items->state(ids[2])->play_count, 3);
}

TEST_F(PlayerLibraryTest, SettingsRoundTripWithLimitsAndBadValues) {
    EXPECT_EQ(items->load_settings()->volume, 100);   // nothing stored: the defaults
    player::Settings s = player::Settings::defaults();
    s.volume = 40;
    s.muted = true;
    s.subtitle_languages = "eng,fre";
    s.viewed_percent = 85;
    ASSERT_TRUE(items->save_settings(s));
    const auto loaded = items->load_settings().value();
    EXPECT_EQ(loaded.volume, 40);
    EXPECT_TRUE(loaded.muted);
    EXPECT_EQ(loaded.subtitle_languages, "eng,fre");
    EXPECT_EQ(loaded.viewed_percent, 85);

    s.volume = 101;
    EXPECT_FALSE(items->save_settings(s).has_value());
    // A value that doesn't parse or is out of range falls back to the default.
    ASSERT_TRUE(db->exec("UPDATE player_settings SET value = 'loud' WHERE key = 'volume';"
                         "UPDATE player_settings SET value = '0' WHERE key = 'viewed_percent';"));
    EXPECT_EQ(items->load_settings()->volume, 100);
    EXPECT_EQ(items->load_settings()->viewed_percent, 90);
}

TEST_F(PlayerLibraryTest, MigrationTwoTurnsSubtitlesOnUnlessChosenSince) {
    EXPECT_TRUE(items->load_settings()->subtitles_on);   // the default since 2026-10-03
    // A database from before: the old default saved along with the volume.
    player::Settings old = player::Settings::defaults();
    old.subtitles_on = false;
    old.volume = 70;
    ASSERT_TRUE(items->save_settings(old));
    // (Migration 3's playlist tables came later: a version-1 database doesn't have them.)
    ASSERT_TRUE(db->exec("DROP TABLE player_playlist_items; DROP TABLE player_playlists;"
                         "UPDATE db_migrations SET version = 1 WHERE component = 'player'"));
    ASSERT_TRUE(player::Library::open(*db).has_value());
    const auto now = items->load_settings().value();
    EXPECT_TRUE(now.subtitles_on);
    EXPECT_EQ(now.volume, 70);   // everything else kept
    // Turned off after that: it stays off (the migration runs once).
    ASSERT_TRUE(items->save_settings(old));
    ASSERT_TRUE(player::Library::open(*db).has_value());
    EXPECT_FALSE(items->load_settings()->subtitles_on);
}

TEST_F(PlayerLibraryTest, ARowGoesWithItsEntry) {
    player::EntryId a = 0;
    for (const auto& e : all_entries(root)) {
        if (e.relative_path == "a.flac") a = e.id;
    }
    ASSERT_TRUE(items->set_liked(a, true, 1));
    fs::remove(dir.path() / "music" / "a.flac");
    set_settings([](file::Settings& s) { s.mass_removal_threshold_percent = 100; });
    ASSERT_EQ(library->sync_root(root).outcome, file::RootSyncOutcome::Synced);
    auto st = db->prepare("SELECT COUNT(*) FROM player_items");
    ASSERT_TRUE(st && st->step().value());
    EXPECT_EQ(st->column_int64(0), 0);
}
