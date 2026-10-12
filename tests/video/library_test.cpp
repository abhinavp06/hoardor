// video::Library: storage and generic queries with synthetic VideoInfo on real entries.

#include <hoardor/file/library.hpp>
#include <hoardor/video/video.hpp>

#include "file/file_time.hpp"
#include "support/library_fixture.hpp"

#include <gtest/gtest.h>

#include <map>
#include <set>

namespace fs = std::filesystem;
using namespace hoardor;
using video::Field;
using video::Filter;
using video::GroupOrder;
using video::Value;

namespace {

constexpr std::int64_t old_mtime = 1'600'000'000'000'000'000;

class VideoLibraryTest : public test::LibraryTest {
protected:
    void SetUp() override {
        LibraryTest::SetUp();
        auto lib = video::Library::open(*db);
        ASSERT_TRUE(lib.has_value()) << lib.error().message;
        videos = std::make_unique<video::Library>(std::move(*lib));
    }

    void put(const std::string& relative, std::size_t size = 1) {
        const auto p = dir.write(fs::path("films") / relative, size);
        fs::last_write_time(p, file::detail::from_unix_ns(old_mtime));
    }

    void sync(std::string_view category_name = "Movies") {
        if (!root) {
            fs::create_directories(dir.path() / "films");
            root = library->add_root(category(category_name), dir.path() / "films", "Films", false)->id;
        }
        ASSERT_EQ(library->sync_root(root).outcome, file::RootSyncOutcome::Synced);
        ids.clear();
        for (const auto& e : all_entries(root)) ids[e.relative_path] = e.id;
    }

    void store(const std::string& relative, const video::VideoInfo& info, video::EntryId poster = 0) {
        ASSERT_TRUE(videos->store(ids.at(relative), 1, old_mtime, info, poster));
    }

    static video::VideoInfo movie(std::string title, int year, int height = 1080, std::string hdr = "") {
        video::VideoInfo v;
        v.type = video::Type::Movie;
        v.title = title;
        v.year = year;
        v.width = height * 16 / 9;
        v.height = height;
        v.hdr = hdr;
        v.duration_ms = 7'200'000;
        v.video_codec = "hevc";
        return v;
    }

    static video::VideoInfo episode(std::string show, int season, int number, std::string title) {
        video::VideoInfo v;
        v.type = video::Type::Episode;
        v.show = show;
        v.season = season;
        v.episode = number;
        v.title = title;
        v.height = 1080;
        v.duration_ms = 3'000'000;
        return v;
    }

    static video::VideoInfo extra(std::string guessed_owner, std::string title) {
        video::VideoInfo v;
        v.type = video::Type::Extra;
        v.show = guessed_owner;
        v.title = title;
        v.height = 1080;
        return v;
    }

    video::Item item(const std::string& relative) { return videos->item(ids.at(relative)).value(); }

    std::unique_ptr<video::Library> videos;
    file::RootId root = 0;
    std::map<std::string, video::EntryId> ids;
};

}

TEST_F(VideoLibraryTest, OneCardPerMovieWhateverTheCopies) {
    put("Arrival 1080p.mkv");
    put("Arrival 2160p.mkv");
    put("Heat.mkv");
    sync();
    store("Arrival 1080p.mkv", movie("Arrival", 2016, 1080));
    store("Arrival 2160p.mkv", movie("Arrival", 2016, 2160, "HDR10"));
    store("Heat.mkv", movie("Heat", 1995, 1080));

    const Filter movies{{{Field::Type, Value{std::int64_t{1}}}}};
    const std::vector<Field> card{Field::Title, Field::Year};
    auto page = videos->groups(card, movies).value();
    ASSERT_EQ(page.items.size(), 2u);
    EXPECT_EQ(page.items[0].values, (std::vector<std::string>{"Arrival", "2016"}));
    EXPECT_EQ(page.items[0].items, 2u);
    EXPECT_EQ(page.items[0].max_height, 2160);
    EXPECT_TRUE(page.items[0].any_hdr);
    EXPECT_FALSE(page.items[1].any_hdr);
    EXPECT_EQ(videos->group_count(card, movies).value(), 2u);

    // The movie page: its copies, best first (the app picks the default).
    const Filter arrival{{{Field::Title, Value{"arrival"}}, {Field::Year, Value{std::int64_t{2016}}}}};
    const std::vector<video::Order> best{{Field::Height, true}};
    const auto copies = videos->items(arrival, best).value().items;
    ASSERT_EQ(copies.size(), 2u);
    EXPECT_EQ(copies[0].height, 2160);
    EXPECT_EQ(copies[0].hdr, "HDR10");
}

TEST_F(VideoLibraryTest, ShowsSeasonsAndEpisodes) {
    for (int s = 1; s <= 2; ++s) {
        for (int e = 1; e <= 3; ++e) put("Dark/S" + std::to_string(s) + "E" + std::to_string(e) + ".mkv");
    }
    put("Fargo/S1E1.mkv");
    sync("Shows");
    for (int s = 1; s <= 2; ++s) {
        for (int e = 3; e >= 1; --e) {  // stored out of order
            store("Dark/S" + std::to_string(s) + "E" + std::to_string(e) + ".mkv", episode("Dark", s, e, "Ep " + std::to_string(e)));
        }
    }
    store("Fargo/S1E1.mkv", episode("Fargo", 1, 1, "The Crocodile's Dilemma"));

    const Filter episodes{{{Field::Type, Value{std::int64_t{2}}}}};
    const auto shows = videos->groups(std::vector<Field>{Field::Show}, episodes).value().items;
    ASSERT_EQ(shows.size(), 2u);
    EXPECT_EQ(shows[0].values[0], "Dark");
    EXPECT_EQ(shows[0].items, 6u);

    const Filter dark{{{Field::Show, Value{"Dark"}}}};
    const auto seasons = videos->groups(std::vector<Field>{Field::Show, Field::Season}, dark).value().items;
    ASSERT_EQ(seasons.size(), 2u);
    EXPECT_EQ(seasons[1].values, (std::vector<std::string>{"Dark", "2"}));
    EXPECT_EQ(seasons[1].items, 3u);

    const Filter season2{{{Field::Show, Value{"Dark"}}, {Field::Season, Value{std::int64_t{2}}}}};
    const std::vector<video::Order> by_episode{{Field::Episode}};
    const auto eps = videos->items(season2, by_episode).value().items;
    ASSERT_EQ(eps.size(), 3u);
    EXPECT_EQ(eps[0].episode, 1);
    EXPECT_EQ(eps[2].episode, 3);
}

TEST_F(VideoLibraryTest, AGroupsFirstEntryIsItsFirstRealEpisode) {
    // For a frame as the poster when nothing else has one (features/posters.md §4).
    for (const char* f : {"Dark/Specials.mkv", "Dark/S2E1.mkv", "Dark/S1E2.mkv", "Dark/S1E1.mkv", "Heat 720p.mkv", "Heat 1080p.mkv"}) put(f);
    sync("Shows");
    store("Dark/Specials.mkv", episode("Dark", 0, 1, "Making of"));
    store("Dark/S2E1.mkv", episode("Dark", 2, 1, "B"));
    store("Dark/S1E2.mkv", episode("Dark", 1, 2, "A2"));
    store("Dark/S1E1.mkv", episode("Dark", 1, 1, "A1"));
    store("Heat 720p.mkv", movie("Heat", 1995, 720));
    store("Heat 1080p.mkv", movie("Heat", 1995, 1080));

    const Filter episodes{{{Field::Type, Value{std::int64_t{2}}}}};
    const auto shows = videos->groups(std::vector<Field>{Field::Show}, episodes).value().items;
    ASSERT_EQ(shows.size(), 1u);
    EXPECT_EQ(shows[0].first_entry, ids.at("Dark/S1E1.mkv"));
    EXPECT_EQ(shows[0].poster_entry, 0);  // nothing else: the app falls back to Plex, then a frame

    const Filter movies{{{Field::Type, Value{std::int64_t{1}}}}};
    const auto heat = videos->groups(std::vector<Field>{Field::Title, Field::Year}, movies).value().items;
    ASSERT_EQ(heat.size(), 1u);
    EXPECT_TRUE(heat[0].first_entry == ids.at("Heat 720p.mkv") || heat[0].first_entry == ids.at("Heat 1080p.mkv"));
}

TEST_F(VideoLibraryTest, NoDirectorOrNoGenreIsAFilterToo) {
    // For a grid "by director": its sections leave out the movies without one, so the grid
    // adds a "no director" section from this filter.
    for (const char* f : {"Heat.mkv", "Home video.mkv", "Arrival.mkv"}) put(f);
    sync();
    video::VideoInfo heat = movie("Heat", 1995);
    heat.directors = {"Michael Mann"};
    heat.genres = {"Crime"};
    store("Heat.mkv", heat);
    store("Home video.mkv", movie("Home video", 0));
    video::VideoInfo arrival = movie("Arrival", 2016);
    arrival.genres = {"Drama"};
    store("Arrival.mkv", arrival);

    const std::vector<Field> card{Field::Title, Field::Year};
    const std::vector<Field> by_director{Field::Director};
    EXPECT_EQ(videos->group_count(by_director, {}).value(), 1u);   // only Michael Mann
    const Filter no_director{{{Field::Director, Value{}, true}}};
    const auto none = videos->groups(card, no_director).value().items;
    ASSERT_EQ(none.size(), 2u);
    EXPECT_EQ(none[0].values[0], "Arrival");
    EXPECT_EQ(none[1].values[0], "Home video");
    const Filter no_genre{{{Field::Genre, Value{}, true}}};
    EXPECT_EQ(videos->count(no_genre).value(), 1u);
    const Filter no_year{{{Field::Year, Value{}, true}}};
    EXPECT_EQ(videos->count(no_year).value(), 1u);
    // With other conditions, and on its own count.
    const Filter both{{{Field::Director, Value{}, true}, {Field::Genre, Value{}, true}}};
    EXPECT_EQ(videos->count(both).value(), 1u);
    const Filter search_none{{{Field::Search, Value{}, true}}};
    EXPECT_FALSE(videos->count(search_none).has_value());
}

TEST_F(VideoLibraryTest, DirectorsGenresAndWriters) {
    put("a.mkv");
    put("b.mkv");
    put("c.mkv");
    sync();
    auto a = movie("Arrival", 2016);
    a.directors = {"Denis Villeneuve"};
    a.genres = {"Science Fiction", "Drama"};
    a.writers = {"Eric Heisserer"};
    auto b = movie("Sicario", 2015);
    b.directors = {"Denis Villeneuve"};
    b.genres = {"Thriller"};
    auto c = movie("Heat", 1995);
    c.directors = {"Michael Mann"};
    c.writers = {"Denis Villeneuve"};  // as a writer, he isn't Heat's director
    store("a.mkv", a);
    store("b.mkv", b);
    store("c.mkv", c);

    const Filter by_director{{{Field::Director, Value{"denis villeneuve"}}}};
    EXPECT_EQ(videos->count(by_director).value(), 2u);
    EXPECT_EQ(videos->count({{{Field::Genre, Value{"drama"}}}}).value(), 1u);
    const auto directors = videos->groups(std::vector<Field>{Field::Director}, {}).value().items;
    ASSERT_EQ(directors.size(), 2u);
    EXPECT_EQ(directors[0].values[0], "Denis Villeneuve");
    EXPECT_EQ(directors[0].items, 2u);
    EXPECT_EQ(videos->item(ids.at("a.mkv"))->directors, a.directors);
}

TEST_F(VideoLibraryTest, PostersComeFromCompanionImagesThatStillExist) {
    put("Arrival/Arrival.mkv");
    put("Arrival/poster.jpg");
    sync();
    store("Arrival/Arrival.mkv", movie("Arrival", 2016), ids.at("Arrival/poster.jpg"));
    EXPECT_EQ(videos->groups(std::vector<Field>{Field::Title}, {}).value().items[0].poster_entry, ids.at("Arrival/poster.jpg"));
    fs::remove(dir.path() / "films" / "Arrival" / "poster.jpg");
    set_settings([](file::Settings& s) { s.mass_removal_threshold_percent = 100; });  // 1 of 2 files is "mass" removal
    sync();
    EXPECT_EQ(videos->item(ids.at("Arrival/Arrival.mkv"))->poster_entry, 0);  // a removed image isn't handed out
}

TEST_F(VideoLibraryTest, PendingAndErrorsAndStreams) {
    put("a.mkv");
    put("b.mkv");
    put("poster.jpg");
    sync();
    EXPECT_EQ(videos->pending_count(std::nullopt).value(), 2u);
    auto a = movie("A", 2000);
    a.audio = {{"jpn", "truehd", 8, "Atmos"}, {"eng", "ac3", 6, "Commentary"}};
    a.subtitles = {{"eng", "subrip", 0, "SDH"}};
    store("a.mkv", a);
    ASSERT_TRUE(videos->store_error(ids.at("b.mkv"), 1, old_mtime, "moov atom not found"));
    EXPECT_EQ(videos->pending_count(std::nullopt).value(), 0u);
    EXPECT_EQ(videos->count({}).value(), 1u);
    const auto back = videos->item(ids.at("a.mkv")).value();
    ASSERT_EQ(back.audio.size(), 2u);
    EXPECT_EQ(back.audio[0].language, "jpn");
    EXPECT_EQ(back.audio[0].channels, 8);
    EXPECT_EQ(back.audio[1].title, "Commentary");
    ASSERT_EQ(back.subtitles.size(), 1u);
    EXPECT_EQ(back.subtitles[0].title, "SDH");
}

TEST_F(VideoLibraryTest, MigrationThreeReadsEveryVideoAgain) {
    put("a.mkv");
    sync();
    store("a.mkv", movie("A", 2000));
    ASSERT_EQ(videos->pending_count(std::nullopt).value(), 0u);
    // A database from before migration 3 (posters cut off at 200 companions).
    ASSERT_TRUE(db->exec("UPDATE db_migrations SET version = 2 WHERE component = 'video'"));
    ASSERT_TRUE(video::Library::open(*db).has_value());
    EXPECT_EQ(videos->pending_count(std::nullopt).value(), 1u);
    EXPECT_EQ(videos->count({}).value(), 1u);  // still listed while it waits
}

TEST_F(VideoLibraryTest, MigrationFourReadsEveryVideoAgain) {
    put("a.mkv");
    sync();
    store("a.mkv", movie("A", 2000));
    // A database from before migration 4 (names found fewer episodes, no extras or samples).
    ASSERT_TRUE(db->exec("UPDATE db_migrations SET version = 3 WHERE component = 'video'"));
    ASSERT_TRUE(video::Library::open(*db).has_value());
    EXPECT_EQ(videos->pending_count(std::nullopt).value(), 1u);
    EXPECT_EQ(videos->count({}).value(), 1u);
}

TEST_F(VideoLibraryTest, PagingGroupsWithoutGapsOrRepeats) {
    for (int i = 0; i < 40; ++i) put("m" + std::to_string(i) + ".mkv");
    sync();
    for (int i = 0; i < 40; ++i) store("m" + std::to_string(i) + ".mkv", movie("Movie " + std::to_string(i), 1960 + i % 25));
    for (GroupOrder order : {GroupOrder::Values, GroupOrder::Year, GroupOrder::AddedLast}) {
        std::set<std::string> seen;
        std::optional<core::Cursor> after;
        std::size_t total = 0;
        while (true) {
            auto page = videos->groups(std::vector<Field>{Field::Title, Field::Year}, {}, order, true, after, 6).value();
            for (const auto& g : page.items) seen.insert(g.values[0]);
            total += page.items.size();
            if (!page.next) break;
            after = page.next;
        }
        EXPECT_EQ(total, 40u);
        EXPECT_EQ(seen.size(), 40u);
    }
}

TEST_F(VideoLibraryTest, UnusedNamesAreRemoved) {
    put("a.mkv");
    sync();
    auto a = movie("A", 2000);
    a.directors = {"X"};
    a.genres = {"Y"};
    store("a.mkv", a);
    ASSERT_TRUE(library->remove_root(root));
    EXPECT_EQ(videos->remove_unused_names().value(), 2u);
}

TEST_F(VideoLibraryTest, SearchTitlesShowsAndDirectors) {
    put("a.mkv");
    put("b.mkv");
    sync();
    auto a = movie("Amélie", 2001);
    a.directors = {"Jean-Pierre Jeunet"};
    store("a.mkv", a);
    auto b = movie("Dark", 2017);
    b.type = video::Type::Episode;
    b.show = "Dark";
    b.title = "Secrets";
    store("b.mkv", b);
    const auto found = [&](const std::string& text) { return videos->count({{{Field::Search, Value{text}}}}).value(); };
    EXPECT_EQ(found("amelie"), 1u);
    EXPECT_EQ(found("jeunet"), 1u);
    EXPECT_EQ(found("dark"), 1u);
    EXPECT_EQ(found("secr"), 1u);
    EXPECT_EQ(found("nothing"), 0u);
}

// features/shows_and_extras.md §3
TEST_F(VideoLibraryTest, AttachMakesSpecialsOfFilmsInAShowsOwnFolder) {
    const std::string dbz = "DRAGON BALL Z (1989-2019) - The COMPLETE Series/";
    put(dbz + "Season 1/Dragon Ball Z - S01E01.mkv");
    put(dbz + "2a. Movies (1989-2018)/Dragon Ball Z - Movie 01 - Dead Zone (1989 BluRay).mkv");
    put(dbz + "2a. Movies (1989-2018)/Described by an nfo.mkv");
    // A director's folder holding a mini-series: its films aren't the series' specials.
    put("Krzysztof Kieslowski/Dekalog (1989)/Dekalog S01E01.mkv");
    put("Krzysztof Kieslowski/Blue (1993)/Blue (1993).mkv");
    put("Arrival (2016)/Arrival (2016).mkv");
    put("Loose (2001).mkv");
    sync("Shows");
    store(dbz + "Season 1/Dragon Ball Z - S01E01.mkv", episode("Dragon Ball Z", 1, 1, "The New Threat"));
    store(dbz + "2a. Movies (1989-2018)/Dragon Ball Z - Movie 01 - Dead Zone (1989 BluRay).mkv",
          movie("Dragon Ball Z - Movie 01 - Dead Zone", 1989));
    auto described = movie("Bardock", 1990);
    described.source = video::Source::Nfo;  // an .nfo said "movie": left alone
    store(dbz + "2a. Movies (1989-2018)/Described by an nfo.mkv", described);
    store("Krzysztof Kieslowski/Dekalog (1989)/Dekalog S01E01.mkv", episode("Dekalog", 1, 1, "One"));
    store("Krzysztof Kieslowski/Blue (1993)/Blue (1993).mkv", movie("Blue", 1993));
    store("Arrival (2016)/Arrival (2016).mkv", movie("Arrival", 2016));
    store("Loose (2001).mkv", movie("Loose", 2001));

    EXPECT_EQ(videos->attach(root).value(), 1u);
    const auto special = item(dbz + "2a. Movies (1989-2018)/Dragon Ball Z - Movie 01 - Dead Zone (1989 BluRay).mkv");
    EXPECT_EQ(special.type, video::Type::Episode);
    EXPECT_EQ(special.season, 0);
    EXPECT_EQ(special.show, "Dragon Ball Z");          // the episodes' spelling, not the folder's
    EXPECT_EQ(special.title, "Movie 01 - Dead Zone");  // without the show's name in front
    EXPECT_EQ(special.year, 1989);
    EXPECT_EQ(item(dbz + "2a. Movies (1989-2018)/Described by an nfo.mkv").type, video::Type::Movie);
    EXPECT_EQ(item("Krzysztof Kieslowski/Blue (1993)/Blue (1993).mkv").type, video::Type::Movie);
    EXPECT_EQ(item("Arrival (2016)/Arrival (2016).mkv").type, video::Type::Movie);
    EXPECT_EQ(item("Loose (2001).mkv").type, video::Type::Movie);

    // The show's page lists it under season 0; a second pass changes nothing.
    const Filter dbz_seasons{{{Field::Type, Value{std::int64_t{2}}}, {Field::Show, Value{"Dragon Ball Z"}}}};
    EXPECT_EQ(videos->count(dbz_seasons).value(), 2u);
    EXPECT_EQ(videos->attach(root).value(), 0u);
}

TEST_F(VideoLibraryTest, AttachNamesExtrasAfterTheirShowOrMovie) {
    const std::string bb = "Breaking Bad (2008) Season 1-5 (1080p BluRay)/";
    put(bb + "Season 1/Breaking Bad (2008) - S01E01 - Pilot.mkv");
    put(bb + "Featurettes/Season 1/Deleted Scenes/Bingo.mkv");
    put("Amelie (2001) (1080p BluRay)/Amelie (2001).mkv", 3);
    put("Amelie (2001) (1080p BluRay)/Amelie (2001) sample cut.mkv", 2);  // smaller: not the main film
    put("Amelie (2001) (1080p BluRay)/Featurettes/Trailer.mkv");
    put("Nobody/Extras/Interview.mkv");
    sync("Movies");
    store(bb + "Season 1/Breaking Bad (2008) - S01E01 - Pilot.mkv", episode("Breaking Bad", 1, 1, "Pilot"));
    store(bb + "Featurettes/Season 1/Deleted Scenes/Bingo.mkv", extra("Breaking Bad", "Deleted Scenes - Bingo"));
    store("Amelie (2001) (1080p BluRay)/Amelie (2001).mkv", movie("Am\xC3\xA9lie", 2001));  // tagged: not the folder's spelling
    store("Amelie (2001) (1080p BluRay)/Amelie (2001) sample cut.mkv", movie("Amelie cut", 2001));
    store("Amelie (2001) (1080p BluRay)/Featurettes/Trailer.mkv", extra("Amelie", "Trailer"));
    store("Nobody/Extras/Interview.mkv", extra("Nobody", "Interview"));

    EXPECT_EQ(videos->attach(root).value(), 1u);  // only the trailer's owner changes
    EXPECT_EQ(item(bb + "Featurettes/Season 1/Deleted Scenes/Bingo.mkv").show, "Breaking Bad");
    const auto trailer = item("Amelie (2001) (1080p BluRay)/Featurettes/Trailer.mkv");
    EXPECT_EQ(trailer.show, "Am\xC3\xA9lie");
    EXPECT_EQ(trailer.year, 2001);
    EXPECT_EQ(item("Nobody/Extras/Interview.mkv").show, "Nobody");  // nothing to tie it to: the guess stays

    // A movie's page finds its extras by its title and year; the grids never see them.
    const Filter amelie_extras{{{Field::Type, Value{std::int64_t{3}}}, {Field::Show, Value{"Am\xC3\xA9lie"}},
                                {Field::Year, Value{std::int64_t{2001}}}}};
    EXPECT_EQ(videos->count(amelie_extras).value(), 1u);
    const Filter movies{{{Field::Type, Value{std::int64_t{1}}}}};
    EXPECT_EQ(videos->count(movies).value(), 2u);
    const Filter episodes{{{Field::Type, Value{std::int64_t{2}}}}};
    EXPECT_EQ(videos->count(episodes).value(), 1u);
}
