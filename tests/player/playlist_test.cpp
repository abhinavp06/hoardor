// player::Library's playlists and liked songs (features/playlists.md §6), on real entries with
// tracks stored by the audio engine.

#include <hoardor/audio/audio.hpp>
#include <hoardor/player/player.hpp>

#include "support/library_fixture.hpp"

#include <gtest/gtest.h>

#include <map>

namespace fs = std::filesystem;
using namespace hoardor;
using player::PlaylistId;

namespace {

class PlaylistTest : public test::LibraryTest {
protected:
    void SetUp() override {
        LibraryTest::SetUp();
        for (const char* name : {"a.flac", "b.flac", "c.flac", "d.flac", "film.mkv"}) dir.write(fs::path("media") / name, 1);
        // Audio and video, so the film is an entry too (one that isn't a track).
        const auto both = library->add_category("Everything", {file::FileKind::Audio, file::FileKind::Video});
        ASSERT_TRUE(both.has_value());
        root = library->add_root(*both, dir.path() / "media", "Media", false)->id;
        ASSERT_EQ(library->sync_root(root).outcome, file::RootSyncOutcome::Synced);
        for (const auto& e : all_entries(root)) ids[e.relative_path] = e.id;
        auto audio = audio::Library::open(*db);
        ASSERT_TRUE(audio.has_value()) << audio.error().message;
        tracks = std::make_unique<audio::Library>(std::move(*audio));
        // a–d are tracks (1, 2, 3, 4 minutes); the film isn't.
        int minutes = 1;
        for (const char* name : {"a.flac", "b.flac", "c.flac", "d.flac"}) {
            audio::TrackInfo t;
            t.title = name;
            t.sample_rate = 44100;
            t.channels = 2;
            t.duration_ms = 60'000 * minutes++;
            const auto e = library->entry(ids.at(name)).value();
            ASSERT_TRUE(tracks->store(e.id, static_cast<std::int64_t>(e.size), e.mtime_ns, t));
        }
        auto lib = player::Library::open(*db);
        ASSERT_TRUE(lib.has_value()) << lib.error().message;
        items = std::make_unique<player::Library>(std::move(*lib));
    }

    std::vector<std::string> names() {
        std::vector<std::string> out;
        const auto all = items->playlists().value();   // kept: a loop over value() would outlive the temporary
        for (const auto& p : all) out.push_back(p.name);
        return out;
    }
    std::vector<player::EntryId> entries(PlaylistId id) { return items->playlist_entries(id).value(); }
    player::EntryId id(const char* name) { return ids.at(name); }

    file::RootId root = 0;
    std::map<std::string, player::EntryId> ids;
    std::unique_ptr<audio::Library> tracks;
    std::unique_ptr<player::Library> items;
};

}

TEST_F(PlaylistTest, CreateRenameDeleteWithNamesThatMustBeUnique) {
    const auto road = items->create_playlist("  Road Trip ", 1).value();
    EXPECT_EQ(items->playlist(road)->name, "Road Trip");   // trimmed
    EXPECT_FALSE(items->create_playlist("road trip", 2).has_value());   // the same name, another case
    EXPECT_FALSE(items->create_playlist("   ", 2).has_value());
    const auto jazz = items->create_playlist("Jazz", 3).value();
    EXPECT_FALSE(items->rename_playlist(jazz, "ROAD TRIP", 4).has_value());
    ASSERT_TRUE(items->rename_playlist(road, "road trip", 5));   // its own name in another case
    EXPECT_EQ(items->playlist(road)->name, "road trip");
    EXPECT_EQ(items->playlist(road)->updated_ns, 5);
    ASSERT_TRUE(items->delete_playlist(road));
    EXPECT_FALSE(items->playlist(road).has_value());
    EXPECT_EQ(names(), (std::vector<std::string>{"Jazz"}));
    EXPECT_FALSE(items->rename_playlist(road, "Gone", 6).has_value());
}

TEST_F(PlaylistTest, NewOnTopPinnedFirstAndTheUsersOrder) {
    const auto a = items->create_playlist("A", 1).value();
    const auto b = items->create_playlist("B", 2).value();
    const auto c = items->create_playlist("C", 3).value();
    EXPECT_EQ(names(), (std::vector<std::string>{"C", "B", "A"}));   // a new one starts on top
    ASSERT_TRUE(items->set_pinned(a, true));
    ASSERT_TRUE(items->set_pinned(b, true));
    EXPECT_EQ(names(), (std::vector<std::string>{"B", "A", "C"}));   // pinned first, the newest pin on top
    EXPECT_TRUE(items->playlist(a)->pinned);
    ASSERT_TRUE(items->move_playlist(b, 1));                        // within the pinned group
    EXPECT_EQ(names(), (std::vector<std::string>{"A", "B", "C"}));
    const auto d = items->create_playlist("D", 4).value();
    EXPECT_EQ(names(), (std::vector<std::string>{"A", "B", "D", "C"}));
    ASSERT_TRUE(items->move_playlist(d, 99));                       // past the end: the end of its group
    EXPECT_EQ(names(), (std::vector<std::string>{"A", "B", "C", "D"}));
    ASSERT_TRUE(items->set_pinned(a, false));                       // to the top of the rest
    EXPECT_EQ(names(), (std::vector<std::string>{"B", "A", "C", "D"}));
    ASSERT_TRUE(items->set_pinned(c, false));                       // already unpinned: nothing moves
    EXPECT_EQ(names(), (std::vector<std::string>{"B", "A", "C", "D"}));
}

TEST_F(PlaylistTest, AddInOrderSkippingWhatsThereAndRefusingNonTracks) {
    const auto p = items->create_playlist("Mix", 1).value();
    const std::vector<player::EntryId> first{id("c.flac"), id("a.flac"), id("film.mkv"), id("a.flac")};
    const auto added = items->add_to_playlist(p, first, 10).value();
    EXPECT_EQ(added.added, 2u);
    EXPECT_EQ(added.skipped, 1u);       // a.flac twice in one call
    EXPECT_EQ(added.not_tracks, 1u);    // the film
    const std::vector<player::EntryId> more{id("a.flac"), id("b.flac")};
    EXPECT_EQ(items->add_to_playlist(p, more, 11)->added, 1u);
    EXPECT_EQ(entries(p), (std::vector<player::EntryId>{id("c.flac"), id("a.flac"), id("b.flac")}));
    const auto info = items->playlist(p).value();
    EXPECT_EQ(info.tracks, 3u);
    EXPECT_EQ(info.duration_ms, 60'000 * (3 + 1 + 2));
    EXPECT_EQ(info.updated_ns, 11);
    EXPECT_EQ(items->add_to_playlist(p, std::vector<player::EntryId>{}, 12)->added, 0u);
    EXPECT_FALSE(items->add_to_playlist(9999, more, 12).has_value());
    // Which playlists hold a file.
    const auto other = items->create_playlist("Other", 13).value();
    ASSERT_TRUE(items->add_to_playlist(other, std::vector<player::EntryId>{id("a.flac")}, 14));
    auto with = items->playlists_with(id("a.flac")).value();
    std::sort(with.begin(), with.end());
    EXPECT_EQ(with, (std::vector<PlaylistId>{p, other}));
    EXPECT_TRUE(items->playlists_with(id("d.flac"))->empty());
}

TEST_F(PlaylistTest, RemoveAndMoveRows) {
    const auto p = items->create_playlist("Mix", 1).value();
    const std::vector<player::EntryId> all{id("a.flac"), id("b.flac"), id("c.flac"), id("d.flac")};
    ASSERT_TRUE(items->add_to_playlist(p, all, 1));
    const auto rows = items->playlist_items(p).value().items;
    ASSERT_EQ(rows.size(), 4u);
    ASSERT_TRUE(items->move_in_playlist(p, rows[3].id, 0, 2));    // d to the top
    EXPECT_EQ(entries(p), (std::vector<player::EntryId>{id("d.flac"), id("a.flac"), id("b.flac"), id("c.flac")}));
    ASSERT_TRUE(items->move_in_playlist(p, rows[0].id, 2, 3));    // a into the middle
    EXPECT_EQ(entries(p), (std::vector<player::EntryId>{id("d.flac"), id("b.flac"), id("a.flac"), id("c.flac")}));
    ASSERT_TRUE(items->move_in_playlist(p, rows[3].id, 99, 4));   // past the end
    EXPECT_EQ(entries(p).back(), id("d.flac"));
    EXPECT_FALSE(items->move_in_playlist(p, 9999, 0, 5).has_value());
    const std::vector<player::PlaylistItemId> gone{rows[1].id, 9999};   // a row of another playlist is ignored
    ASSERT_TRUE(items->remove_from_playlist(p, gone, 6));
    EXPECT_EQ(entries(p), (std::vector<player::EntryId>{id("a.flac"), id("c.flac"), id("d.flac")}));
    EXPECT_EQ(items->playlist(p)->updated_ns, 6);
}

TEST_F(PlaylistTest, PagesInOrder) {
    const auto p = items->create_playlist("Mix", 1).value();
    const std::vector<player::EntryId> all{id("a.flac"), id("b.flac"), id("c.flac"), id("d.flac")};
    ASSERT_TRUE(items->add_to_playlist(p, all, 1));
    std::vector<player::EntryId> seen;
    std::optional<core::Cursor> after;
    int pages = 0;
    do {
        const auto page = items->playlist_items(p, after, 3).value();
        for (const auto& row : page.items) seen.push_back(row.entry);
        after = page.next;
        ++pages;
    } while (after);
    EXPECT_EQ(seen, all);
    EXPECT_EQ(pages, 2);
}

TEST_F(PlaylistTest, AFileLeavingTheLibraryLeavesItsPlaylists) {
    const auto p = items->create_playlist("Mix", 1).value();
    const std::vector<player::EntryId> all{id("a.flac"), id("b.flac")};
    ASSERT_TRUE(items->add_to_playlist(p, all, 1));
    fs::remove(dir.path() / "media" / "a.flac");
    ASSERT_EQ(library->sync_root(root).outcome, file::RootSyncOutcome::Synced);
    EXPECT_EQ(entries(p), (std::vector<player::EntryId>{id("b.flac")}));
    EXPECT_EQ(items->playlist(p)->tracks, 1u);
    // Deleting the playlist deletes its rows, and nothing else.
    ASSERT_TRUE(items->delete_playlist(p));
    EXPECT_TRUE(items->playlists_with(id("b.flac"))->empty());
    EXPECT_TRUE(library->entry(id("b.flac")).has_value());
}

TEST_F(PlaylistTest, LikedSongsAreTracksOnlyNewestFirst) {
    ASSERT_TRUE(items->set_liked(id("a.flac"), true, 10));
    ASSERT_TRUE(items->set_liked(id("c.flac"), true, 30));
    ASSERT_TRUE(items->set_liked(id("b.flac"), true, 20));
    ASSERT_TRUE(items->set_liked(id("film.mkv"), true, 40));   // liked, but not a song
    ASSERT_TRUE(items->set_liked(id("b.flac"), false, 50));
    EXPECT_EQ(items->liked_count().value(), 2u);
    EXPECT_EQ(items->liked_entries().value(), (std::vector<player::EntryId>{id("c.flac"), id("a.flac")}));
    const auto first = items->liked(std::nullopt, 1).value();
    ASSERT_EQ(first.items.size(), 1u);
    EXPECT_EQ(first.items[0].entry, id("c.flac"));
    EXPECT_EQ(first.items[0].liked_ns, 30);
    ASSERT_TRUE(first.next.has_value());
    const auto second = items->liked(first.next, 1).value();
    ASSERT_EQ(second.items.size(), 1u);
    EXPECT_EQ(second.items[0].entry, id("a.flac"));
}
