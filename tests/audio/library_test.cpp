// audio::Library: pending work, storage, and the generic queries. Tracks are stored from
// synthetic TrackInfo against real (synced) entries, so no media files are needed.

#include <hoardor/audio/audio.hpp>
#include <hoardor/file/library.hpp>

#include "file/file_time.hpp"
#include "support/library_fixture.hpp"

#include <gtest/gtest.h>

#include <map>
#include <set>

namespace fs = std::filesystem;
using namespace hoardor;
using audio::Field;
using audio::Filter;
using audio::GroupOrder;
using audio::Order;
using audio::Value;

namespace {

constexpr std::int64_t old_mtime = 1'600'000'000'000'000'000;  // 2020: settled

class AudioLibraryTest : public test::LibraryTest {
protected:
    void SetUp() override {
        LibraryTest::SetUp();
        auto lib = audio::Library::open(*db);
        ASSERT_TRUE(lib.has_value()) << lib.error().message;
        tracks = std::make_unique<audio::Library>(std::move(*lib));
    }

    fs::path put(const std::string& relative, std::size_t size = 1) {
        const auto p = dir.write(fs::path("music") / relative, size);
        fs::last_write_time(p, file::detail::from_unix_ns(old_mtime));
        return p;
    }

    file::RootId sync_music() {
        if (!root) {
            fs::create_directories(dir.path() / "music");
            root = library->add_root(category("Music"), dir.path() / "music", "Music", false)->id;
        }
        EXPECT_EQ(library->sync_root(root).outcome, file::RootSyncOutcome::Synced);
        ids.clear();
        for (const auto& e : all_entries(root)) ids[e.relative_path] = e;
        return root;
    }

    void store(const std::string& relative, audio::TrackInfo info) {
        const file::Entry& e = ids.at(relative);
        ASSERT_TRUE(tracks->store(e.id, static_cast<std::int64_t>(e.size), e.mtime_ns, info));
    }

    static audio::TrackInfo info(std::string album_artist, std::string album, int disc, int track, std::string title,
                                 int year = 2000) {
        audio::TrackInfo t;
        t.album_artist = album_artist;
        t.artists = {album_artist};
        t.album = album;
        t.disc = disc;
        t.track = track;
        t.title = title;
        t.year = year;
        t.duration_ms = 1000;
        t.codec = "flac";
        t.lossless = true;
        t.sample_rate = 44100;
        t.bit_depth = 16;
        return t;
    }

    std::vector<audio::Track> all_tracks(const Filter& filter, std::vector<Order> order, std::size_t page_size = 7) {
        std::vector<audio::Track> out;
        std::optional<core::Cursor> after;
        while (true) {
            auto page = tracks->tracks(filter, order, after, page_size);
            EXPECT_TRUE(page.has_value()) << page.error().message;
            out.insert(out.end(), page->items.begin(), page->items.end());
            if (!page->next) return out;
            after = page->next;
        }
    }

    std::vector<audio::Group> all_groups(std::vector<Field> by, const Filter& filter, GroupOrder order, bool desc,
                                         std::size_t page_size = 7) {
        std::vector<audio::Group> out;
        std::optional<core::Cursor> after;
        while (true) {
            auto page = tracks->groups(by, filter, order, desc, after, page_size);
            EXPECT_TRUE(page.has_value()) << page.error().message;
            out.insert(out.end(), page->items.begin(), page->items.end());
            if (!page->next) return out;
            after = page->next;
        }
    }

    std::unique_ptr<audio::Library> tracks;
    file::RootId root = 0;
    std::map<std::string, file::Entry> ids;
};

}

TEST_F(AudioLibraryTest, PendingListsSettledUnreadAudioOnly) {
    put("a.flac");
    put("b.mp3");
    put("cover.jpg");
    dir.write("music/new.flac", 1);  // modified just now: unsettled
    sync_music();
    auto pending = tracks->pending(std::nullopt).value();
    std::set<std::string> names;
    for (const auto& p : pending) names.insert(p.relative_path);
    EXPECT_EQ(names, (std::set<std::string>{"a.flac", "b.mp3"}));
    EXPECT_EQ(tracks->pending_count(std::nullopt).value(), 2u);
    EXPECT_EQ(tracks->pending_count(category("Movies")).value(), 0u);
    EXPECT_EQ(tracks->pending_count(category("Music")).value(), 2u);

    store("a.flac", info("X", "Y", 1, 1, "a"));
    EXPECT_EQ(tracks->pending_count(std::nullopt).value(), 1u);
    // Paging through pending work.
    EXPECT_EQ(tracks->pending(std::nullopt, 0, 1).value().size(), 1u);
}

TEST_F(AudioLibraryTest, ChangedFilesBecomePendingAgain) {
    put("a.flac");
    sync_music();
    store("a.flac", info("X", "Y", 1, 1, "a"));
    EXPECT_EQ(tracks->pending_count(std::nullopt).value(), 0u);
    put("a.flac", 50);  // new size
    sync_music();
    EXPECT_EQ(tracks->pending_count(std::nullopt).value(), 1u);
    store("a.flac", info("X", "Y", 1, 1, "a v2"));
    EXPECT_EQ(tracks->pending_count(std::nullopt).value(), 0u);
    EXPECT_EQ(tracks->track(ids.at("a.flac").id)->title, "a v2");
}

TEST_F(AudioLibraryTest, OfflineRootsHaveNoPendingWorkButStillList) {
    put("a.flac");
    put("b.flac");
    sync_music();
    store("a.flac", info("X", "Y", 1, 1, "a"));
    fs::rename(dir.path() / "music", dir.path() / "gone");
    library->sync_root(root);  // offline now
    EXPECT_EQ(tracks->pending_count(std::nullopt).value(), 0u);
    const auto t = tracks->tracks({}).value().items;
    ASSERT_EQ(t.size(), 1u);
    EXPECT_FALSE(t[0].root_online);
}

TEST_F(AudioLibraryTest, UnreadableFilesAreRecordedNotListedNotRetried) {
    put("a.flac");
    sync_music();
    ASSERT_TRUE(tracks->store_error(ids.at("a.flac").id, 1, old_mtime, "Invalid data found"));
    EXPECT_EQ(tracks->pending_count(std::nullopt).value(), 0u);
    EXPECT_EQ(tracks->count({}).value(), 0u);
    EXPECT_TRUE(tracks->tracks({}).value().items.empty());
    put("a.flac", 9);  // fixed and copied again: worth another try
    sync_music();
    EXPECT_EQ(tracks->pending_count(std::nullopt).value(), 1u);
}

TEST_F(AudioLibraryTest, StoredFieldsComeBack) {
    put("a.flac", 123);
    sync_music();
    audio::TrackInfo t = info("The Beatles", "Abbey Road", 1, 7, "Here Comes the Sun", 1969);
    t.artists = {"The Beatles", "George Harrison"};
    t.genres = {"Rock", "Pop"};
    t.date = "1969-09-26";
    t.bitrate_kbps = 900;
    t.has_embedded_cover = true;
    t.track_total = 17;
    store("a.flac", t);
    const auto back = tracks->track(ids.at("a.flac").id).value();
    EXPECT_EQ(back.title, "Here Comes the Sun");
    EXPECT_EQ(back.artists, t.artists);
    EXPECT_EQ(back.genres, t.genres);
    EXPECT_EQ(back.date, "1969-09-26");
    EXPECT_EQ(back.year, 1969);
    EXPECT_EQ(back.track, 7);
    EXPECT_EQ(back.track_total, 17);
    EXPECT_EQ(back.bitrate_kbps, 900);
    EXPECT_TRUE(back.has_embedded_cover);
    EXPECT_TRUE(back.root_online);
    EXPECT_EQ(back.size, 123u);
    EXPECT_GT(back.added_ns, 0);
    EXPECT_FALSE(tracks->track(4242).has_value());
}

TEST_F(AudioLibraryTest, FilterIgnoresCaseAndArticles_OrderByDiscAndTrack) {
    for (int i = 1; i <= 4; ++i) put("abbey/" + std::to_string(i) + ".flac");
    put("other.flac");
    sync_music();
    store("abbey/1.flac", info("The Beatles", "Abbey Road", 2, 1, "Sun King"));
    store("abbey/2.flac", info("The Beatles", "Abbey Road", 1, 2, "Something"));
    store("abbey/3.flac", info("Beatles", "abbey road", 1, 1, "Come Together"));
    store("abbey/4.flac", info("The Beatles", "Abbey Road", 1, 10, "Track Ten"));
    store("other.flac", info("Abba", "Arrival", 1, 1, "Dancing Queen"));

    const Filter album{{{Field::AlbumArtist, Value{"beatles"}}, {Field::Album, Value{"ABBEY ROAD"}}}};
    const auto t = all_tracks(album, {{Field::Disc}, {Field::Track}});
    std::vector<std::string> titles;
    for (const auto& x : t) titles.push_back(x.title);
    EXPECT_EQ(titles, (std::vector<std::string>{"Come Together", "Something", "Track Ten", "Sun King"}));
    EXPECT_EQ(tracks->count(album).value(), 4u);
}

TEST_F(AudioLibraryTest, PagingVisitsEveryTrackOnceInEveryOrder) {
    for (int i = 0; i < 50; ++i) put("t" + std::to_string(i) + ".flac");
    sync_music();
    for (int i = 0; i < 50; ++i) {
        store("t" + std::to_string(i) + ".flac", info("Artist " + std::to_string(i % 5), "Album " + std::to_string(i % 9), 1,
                                                       i % 13, "Title " + std::to_string(i), 1990 + i % 7));
    }
    for (const auto& order : std::vector<std::vector<Order>>{{},
                                                             {{Field::Title}},
                                                             {{Field::Year, true}, {Field::Title}},
                                                             {{Field::Added, true}},
                                                             {{Field::AlbumArtist}, {Field::Album, true}, {Field::Track}}}) {
        const auto t = all_tracks({}, order, 7);
        std::set<audio::EntryId> seen;
        for (const auto& x : t) seen.insert(x.entry_id);
        EXPECT_EQ(t.size(), 50u);
        EXPECT_EQ(seen.size(), 50u);
    }
    // Natural order: "Title 9" before "Title 10".
    const auto by_title = all_tracks({}, {{Field::Title}});
    EXPECT_EQ(by_title[9].title, "Title 9");
    EXPECT_EQ(by_title[10].title, "Title 10");
}

TEST_F(AudioLibraryTest, AlbumsAreGroupsInEveryOrder) {
    // 30 albums by 6 artists, 2 tracks each; added order = album index.
    for (int a = 0; a < 30; ++a) {
        for (int t = 1; t <= 2; ++t) put("al" + std::to_string(a) + "/" + std::to_string(t) + ".flac");
    }
    sync_music();
    for (int a = 0; a < 30; ++a) {
        for (int t = 1; t <= 2; ++t) {
            store("al" + std::to_string(a) + "/" + std::to_string(t) + ".flac",
                  info("Artist " + std::to_string(a % 6), "Album " + std::to_string(a), 1, t, "T", 1970 + a));
        }
    }
    const std::vector<Field> album{Field::AlbumArtist, Field::Album};
    EXPECT_EQ(tracks->group_count(album, {}).value(), 30u);

    const auto by_values = all_groups(album, {}, GroupOrder::Values, false);
    ASSERT_EQ(by_values.size(), 30u);
    EXPECT_EQ(by_values[0].values, (std::vector<std::string>{"Artist 0", "Album 0"}));
    EXPECT_EQ(by_values[1].values, (std::vector<std::string>{"Artist 0", "Album 6"}));
    EXPECT_EQ(by_values[0].tracks, 2u);
    EXPECT_EQ(by_values[0].duration_ms, 2000);
    EXPECT_TRUE(by_values[0].any_online);

    const auto by_year_desc = all_groups(album, {}, GroupOrder::Year, true);
    ASSERT_EQ(by_year_desc.size(), 30u);
    EXPECT_EQ(by_year_desc[0].year_max, 1999);
    EXPECT_EQ(by_year_desc[29].year_max, 1970);

    const auto by_added = all_groups(album, {}, GroupOrder::AddedLast, true, 4);
    std::set<std::string> unique;
    for (const auto& g : by_added) unique.insert(g.values[1]);
    EXPECT_EQ(unique.size(), 30u);

    // An artist's albums.
    const Filter artist{{{Field::AlbumArtist, Value{"Artist 2"}}}};
    EXPECT_EQ(all_groups(album, artist, GroupOrder::Values, false).size(), 5u);
    // Artists.
    EXPECT_EQ(tracks->group_count(std::vector<Field>{Field::AlbumArtist}, {}).value(), 6u);
}

TEST_F(AudioLibraryTest, ManyArtistsAndGenresPerTrack) {
    put("a.flac");
    put("b.flac");
    put("c.flac");
    sync_music();
    auto a = info("Main", "Duets", 1, 1, "One");
    a.artists = {"Main", "Guest"};
    a.genres = {"Jazz", "Soul"};
    auto b = info("Main", "Duets", 1, 2, "Two");
    b.genres = {"Jazz"};
    auto c = info("Guest", "Solo", 1, 1, "Alone");
    c.genres = {"soul"};  // same genre, other case
    store("a.flac", a);
    store("b.flac", b);
    store("c.flac", c);

    // "Appears on": Guest's tracks include Main's duet.
    EXPECT_EQ(tracks->count({{{Field::Artist, Value{"guest"}}}}).value(), 2u);
    const auto appears = all_groups({Field::AlbumArtist, Field::Album}, {{{Field::Artist, Value{"Guest"}}}}, GroupOrder::Values, false);
    ASSERT_EQ(appears.size(), 2u);
    EXPECT_EQ(appears[0].values[1], "Solo");  // "Guest" sorts before "Main"

    // Genres as groups (a genre page lists albums filtered by it).
    const auto genres = all_groups({Field::Genre}, {}, GroupOrder::Values, false);
    ASSERT_EQ(genres.size(), 2u);
    EXPECT_EQ(genres[0].values[0], "Jazz");
    EXPECT_EQ(genres[0].tracks, 2u);
    EXPECT_EQ(genres[1].values[0], "Soul");  // shown as first seen
    EXPECT_EQ(genres[1].tracks, 2u);
    EXPECT_EQ(tracks->count({{{Field::Genre, Value{"SOUL"}}}}).value(), 2u);
}

TEST_F(AudioLibraryTest, CopiesInTwoQualitiesAreOneGroup) {
    for (int t = 1; t <= 3; ++t) {
        put("cd/" + std::to_string(t) + ".flac");
        put("hires/" + std::to_string(t) + ".flac");
    }
    sync_music();
    for (int t = 1; t <= 3; ++t) {
        store("cd/" + std::to_string(t) + ".flac", info("A", "Record", 1, t, "S" + std::to_string(t)));
        auto hi = info("A", "Record", 1, t, "S" + std::to_string(t));
        hi.sample_rate = 96000;
        hi.bit_depth = 24;
        hi.has_embedded_cover = true;
        store("hires/" + std::to_string(t) + ".flac", hi);
    }
    const auto groups = all_groups({Field::AlbumArtist, Field::Album}, {}, GroupOrder::Values, false);
    ASSERT_EQ(groups.size(), 1u);
    EXPECT_EQ(groups[0].tracks, 6u);  // grouping copies is the app's job
    // The cover comes from a track that has embedded art.
    EXPECT_EQ(groups[0].cover_entry, ids.at("hires/1.flac").id);
    EXPECT_EQ(tracks->count({{{Field::BitDepth, Value{24}}, {Field::SampleRate, Value{96000}}}}).value(), 3u);
    EXPECT_EQ(tracks->count({{{Field::Codec, Value{"flac"}}}}).value(), 6u);
}

TEST_F(AudioLibraryTest, RemovingFilesRemovesTheirTracksAndUnusedNames) {
    put("a.flac");
    sync_music();
    auto a = info("Gone", "Away", 1, 1, "x");
    a.genres = {"Rare"};
    store("a.flac", a);
    ASSERT_TRUE(library->remove_root(root));
    EXPECT_EQ(tracks->count({}).value(), 0u);
    EXPECT_EQ(tracks->remove_unused_names().value(), 2u);  // "Gone" (artist) and "Rare" (genre)
    EXPECT_EQ(tracks->remove_unused_names().value(), 0u);
}

TEST_F(AudioLibraryTest, CategoryAndRootFilters) {
    put("a.flac");
    sync_music();
    store("a.flac", info("A", "B", 1, 1, "x"));
    EXPECT_EQ(tracks->count({{{Field::Category, Value{category("Music")}}}}).value(), 1u);
    EXPECT_EQ(tracks->count({{{Field::Category, Value{category("Movies")}}}}).value(), 0u);
    EXPECT_EQ(tracks->count({{{Field::Root, Value{root}}}}).value(), 1u);
}

TEST_F(AudioLibraryTest, BadRequestsAreErrors) {
    put("a.flac");
    sync_music();
    store("a.flac", info("A", "B", 1, 1, "x"));
    EXPECT_FALSE(tracks->tracks({}, std::vector<Order>{{Field::Genre}}).has_value());  // can't order by genre
    EXPECT_FALSE(tracks->count({{{Field::Year, Value{"nineteen"}}}}).has_value());
    EXPECT_EQ(tracks->count({{{Field::Year, Value{"2000"}}}}).value(), 1u);  // numbers as text are fine
    auto page = tracks->tracks({}, std::vector<Order>{{Field::Title}}, std::nullopt, 1);
    ASSERT_TRUE(page.has_value());
    ASSERT_TRUE(page->next.has_value());
    // A cursor from another order doesn't fit.
    EXPECT_FALSE(tracks->tracks({}, std::vector<Order>{{Field::Title}, {Field::Year}}, page->next, 1).has_value());
    EXPECT_FALSE(tracks->tracks({}, {}, core::Cursor{"garbage", 1}, 1).has_value());
    EXPECT_FALSE(tracks->groups({}, {}).has_value());  // nothing to group by
}

TEST_F(AudioLibraryTest, SearchFindsWordPrefixesAcrossFieldsIgnoringAccents) {
    put("a.flac");
    put("b.flac");
    put("c.flac");
    put("d.flac");
    sync_music();
    auto a = info("Radiohead", "A Moon Shaped Pool", 1, 1, "Daydreaming");
    a.genres = {"Art Rock"};
    store("a.flac", a);
    store("b.flac", info("Björk", "Homogenic", 1, 1, "Jóga"));
    auto c = info("Various", "Radio Hits", 1, 1, "Signal");
    c.artists = {"AC/DC"};
    store("c.flac", c);
    ASSERT_TRUE(tracks->store_error(ids.at("d.flac").id, 1, old_mtime, "unreadable"));

    const auto found = [&](const std::string& text) { return tracks->count({{{Field::Search, Value{text}}}}).value(); };
    EXPECT_EQ(found("radio"), 2u);      // "Radiohead" (album artist) and "Radio Hits" (album)
    EXPECT_EQ(found("radio hits"), 1u); // every word required
    EXPECT_EQ(found("daydr"), 1u);      // a prefix of the title
    EXPECT_EQ(found("art rock"), 1u);   // genres
    EXPECT_EQ(found("bjork"), 1u);      // diacritics folded
    EXPECT_EQ(found("JOGA"), 1u);
    EXPECT_EQ(found("ac/dc"), 1u);      // punctuation splits words; no FTS syntax leaks through
    EXPECT_EQ(found("\"unbalanced"), 0u);
    EXPECT_EQ(found("   "), 0u);        // no words: nothing
    EXPECT_EQ(found("unreadable"), 0u); // unreadable files aren't indexed

    // Albums matching, grouped as usual.
    const auto albums = all_groups({Field::AlbumArtist, Field::Album}, {{{Field::Search, Value{"radio"}}}}, GroupOrder::Values, false);
    EXPECT_EQ(albums.size(), 2u);

    // Retagging updates the index; removing the files empties it.
    store("a.flac", info("Radiohead", "Kid A", 1, 1, "Everything In Its Right Place"));
    EXPECT_EQ(found("daydr"), 0u);
    EXPECT_EQ(found("everything"), 1u);
    ASSERT_TRUE(library->remove_root(root));
    EXPECT_EQ(found("radio"), 0u);
    EXPECT_EQ(found("bjork"), 0u);
}

TEST_F(AudioLibraryTest, MigrationThreeRetriesRejectedTracksOnce) {
    put("a.flac");
    put("b.flac");
    sync_music();
    ASSERT_TRUE(tracks->store_error(ids.at("a.flac").id, 1, old_mtime, "not a playable audio file"));
    ASSERT_TRUE(tracks->store_error(ids.at("b.flac").id, 1, old_mtime, "Invalid data found when processing input"));
    ASSERT_EQ(tracks->pending_count(std::nullopt).value(), 0u);
    // A database from before migration 3.
    ASSERT_TRUE(db->exec("UPDATE db_migrations SET version = 2 WHERE component = 'audio'"));
    ASSERT_TRUE(audio::Library::open(*db).has_value());
    const auto pending = tracks->pending(std::nullopt, 0, 10).value();
    ASSERT_EQ(pending.size(), 1u);   // only the one rejected for having no length
    EXPECT_EQ(pending[0].entry_id, ids.at("a.flac").id);
}

