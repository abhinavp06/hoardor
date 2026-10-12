#include "video/sources.hpp"

#include "support/temp_dir.hpp"

#include <gtest/gtest.h>

#include <fstream>

namespace fs = std::filesystem;
using namespace hoardor::video::detail;

TEST(Nfo, Movie) {
    const auto d = parse_nfo(R"(<?xml version="1.0" encoding="UTF-8"?>
<movie>
  <title>Arrival</title>
  <year>2016</year>
  <premiered>2016-11-11</premiered>
  <genre>Science Fiction</genre>
  <genre>Drama / Mystery</genre>
  <director>Denis Villeneuve</director>
  <credits>Eric Heisserer</credits>
  <plot>A linguist works with the military.</plot>
  <runtime>116</runtime>
</movie>)");
    ASSERT_TRUE(d.has_value());
    EXPECT_EQ(d->kind, Described::Kind::Movie);
    EXPECT_EQ(d->title, "Arrival");
    EXPECT_EQ(d->year, 2016);
    EXPECT_EQ(d->date, "2016-11-11");
    EXPECT_EQ(d->genres, (std::vector<std::string>{"Science Fiction", "Drama", "Mystery"}));
    EXPECT_EQ(d->directors, (std::vector<std::string>{"Denis Villeneuve"}));
    EXPECT_EQ(d->writers, (std::vector<std::string>{"Eric Heisserer"}));
    EXPECT_EQ(d->runtime_minutes, 116);
}

TEST(Nfo, EpisodeAndShow) {
    const auto e = parse_nfo("<episodedetails><title>Ghosts</title><showtitle>Dark</showtitle><season>1</season>"
                             "<episode>3</episode><aired>2017-12-01</aired><director>Baran bo Odar</director></episodedetails>");
    ASSERT_TRUE(e.has_value());
    EXPECT_EQ(e->kind, Described::Kind::Episode);
    EXPECT_EQ(e->show, "Dark");
    EXPECT_EQ(e->season, 1);
    EXPECT_EQ(e->episode, 3);
    EXPECT_EQ(e->year, 2017);  // from the air date
    const auto s = parse_nfo("<tvshow><title>Dark</title><premiered>2017-12-01</premiered><genre>Mystery</genre></tvshow>");
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(s->kind, Described::Kind::Show);
    EXPECT_EQ(s->year, 2017);
    const auto special = parse_nfo("<episodedetails><title>X</title><season>0</season><episode>1</episode></episodedetails>");
    EXPECT_EQ(special->season, 0);
}

TEST(Nfo, KodiUrlAfterTheXmlAndBrokenFiles) {
    const auto d = parse_nfo("<movie><title>Heat</title></movie>\nhttps://www.themoviedb.org/movie/949\n");
    ASSERT_TRUE(d.has_value());
    EXPECT_EQ(d->title, "Heat");
    EXPECT_FALSE(parse_nfo("https://www.imdb.com/title/tt0113277/").has_value());  // link-only
    EXPECT_FALSE(parse_nfo("<movie><title>Unclosed</movie>").has_value());
    EXPECT_FALSE(parse_nfo("<musicvideo><title>x</title></musicvideo>").has_value());
    EXPECT_FALSE(parse_nfo("").has_value());
}

TEST(Nfo, ReadsFromDiskIncludingUtf8) {
    hoardor::test::TempDir dir;
    const fs::path f = dir.path() / "m.nfo";
    std::ofstream(f, std::ios::binary) << "<movie><title>Am\xC3\xA9lie</title><year>2001</year></movie>";
    const auto d = read_nfo(f);
    ASSERT_TRUE(d.has_value());
    EXPECT_EQ(d->title, "Am\xC3\xA9lie");
    EXPECT_FALSE(read_nfo(dir.path() / "missing.nfo").has_value());
}

TEST(Names, Episodes) {
    auto d = from_name("Shows/Dark/Season 1/Dark.S01E03.Ghosts.1080p.mkv");
    EXPECT_EQ(d.kind, Described::Kind::Episode);
    EXPECT_EQ(d.season, 1);
    EXPECT_EQ(d.episode, 3);
    EXPECT_EQ(d.show, "Dark");  // the folder above "Season 1"
    EXPECT_EQ(d.title, "Ghosts");  // release words dropped

    d = from_name("tv/The Bear - s02e10 - Omelette.mkv");
    EXPECT_EQ(d.kind, Described::Kind::Episode);
    EXPECT_EQ(d.show, "The Bear");
    EXPECT_EQ(d.season, 2);
    EXPECT_EQ(d.episode, 10);
    EXPECT_EQ(d.title, "Omelette");

    d = from_name("Fargo/fargo.3x07.mkv");
    EXPECT_EQ(d.season, 3);
    EXPECT_EQ(d.episode, 7);
    EXPECT_EQ(d.show, "Fargo");  // the folder's spelling of the same name

    d = from_name("Andor/Season 2/03 - Harvest.mkv");
    EXPECT_EQ(d.kind, Described::Kind::Episode);
    EXPECT_EQ(d.season, 2);
    EXPECT_EQ(d.episode, 3);
    EXPECT_EQ(d.show, "Andor");
    EXPECT_EQ(d.title, "Harvest");

    d = from_name("Dark/Specials/E01.mkv");
    EXPECT_EQ(d.season, 0);
    EXPECT_EQ(d.episode, 1);
}

TEST(Names, Movies) {
    auto d = from_name("Movies/Arrival (2016)/Arrival (2016).mkv");
    EXPECT_EQ(d.kind, Described::Kind::Movie);
    EXPECT_EQ(d.title, "Arrival");
    EXPECT_EQ(d.year, 2016);

    d = from_name("Blade.Runner.2049.2017.2160p.UHD.BluRay.mkv");
    EXPECT_EQ(d.title, "Blade Runner 2049");  // the last plausible year wins over a title number
    EXPECT_EQ(d.year, 2017);

    d = from_name("2001 A Space Odyssey [1968].mkv");
    EXPECT_EQ(d.title, "2001 A Space Odyssey");
    EXPECT_EQ(d.year, 1968);

    d = from_name("home_video_final.mp4");
    EXPECT_EQ(d.title, "home video final");
    EXPECT_EQ(d.year, 0);
}

TEST(Names, SeasonFoldersAndCleaning) {
    EXPECT_EQ(season_of_folder("Season 1"), 1);
    EXPECT_EQ(season_of_folder("season.02"), 2);
    EXPECT_EQ(season_of_folder("S03"), 3);
    EXPECT_EQ(season_of_folder("Series 4"), 4);
    EXPECT_EQ(season_of_folder("Specials"), 0);
    EXPECT_EQ(season_of_folder("Seasoning"), -1);
    EXPECT_EQ(season_of_folder("Extras Galore"), -1);
    EXPECT_EQ(clean_name("The.Matrix_Reloaded - "), "The Matrix Reloaded");
    EXPECT_EQ(clean_name("  a  b  "), "a b");
    EXPECT_EQ(clean_name("Evangelion.3.33.You.Can.(Not).Redo"), "Evangelion 3.33 You Can (Not) Redo");
}

// The user's real layouts (features/shows_and_extras.md §2): each of these was a "movie"
// before, so whole shows were missing from a Shows category.
TEST(Names, EpisodesFromReleaseLayouts) {
    const fs::path root = "Shows";
    struct Case {
        const char* path;
        const char* show;
        int season, episode;
        const char* title;
    };
    const Case cases[] = {
        {"[Anime Time] Neon Genesis Evangelion Complete Series [BD] [Dual Audio][1080p]/[Anime Time] Neon Genesis Evangelion - 01.mkv",
         "Neon Genesis Evangelion", 1, 1, ""},
        {"[SubGroup] One Piece/[SubGroup] One Piece - 1001 [1080p].mkv", "One Piece", 1, 1001, ""},
        {"DBZ/Dragon Ball The Complete Series (1995-2003)/Episode 12 A Wish to the Eternal Dragon.mkv", "Dragon Ball", 1, 12,
         "A Wish to the Eternal Dragon"},
        {"The Bads of Bollywood (2025) Hindi 1080p WEBRip x265/The Bads Of Bollywood E03.mkv", "The Bads Of Bollywood", 1, 3, ""},
        {"Breaking Bad (2008) Season 1-5 S01-S05 (1080p BluRay x265 HEVC 10bit AAC 5.1 Silence)/Season 2/"
         "Breaking Bad (2008) - S02E04 - Down (1080p BluRay x265 Silence).mkv",
         "Breaking Bad", 2, 4, "Down"},
        {"Odd Taxi S01 1080p Dual Audio BDRip 10 bits DD+ x265-EMBER/S01E02-How to Spend a Long Night [046DFA4B].mkv", "Odd Taxi", 1, 2,
         "How to Spend a Long Night"},
        {"COURAGE the COWARDLY DOG (1999-2002) - Complete/Season 2 (2000-01)/Courage the Cowardly Dog - S02 E01-E02 - Freaky Fred.mp4",
         "Courage the Cowardly Dog", 2, 1, "Freaky Fred"},
        {"Show/Season 1/Show S01 - E05.mkv", "Show", 1, 5, ""},
        {"COURAGE the COWARDLY DOG (1999-2002)/Specials (1995-2014)/"
         "Courage the Cowardly Dog - S00 E02 - The Wind Whispers Courage (2002 - 480p).mp4",
         "Courage the Cowardly Dog", 0, 2, "The Wind Whispers Courage"},
        {"Doctor Who/Season 2005/Doctor Who S2005E01.mkv", "Doctor Who", 2005, 1, ""},
        // The folders' spelling only when the file's is all lowercase.
        {"Adolescence S01 1080p WEB H264/Adolescence.S01E01.1080p.WEB/adolescence.s01e01.1080p.web.h264-crab.mkv", "Adolescence", 1, 1,
         ""},
    };
    for (const Case& c : cases) {
        const auto d = from_name(root / c.path, root);
        EXPECT_EQ(d.kind, Described::Kind::Episode) << c.path;
        EXPECT_EQ(d.show, c.show) << c.path;
        EXPECT_EQ(d.season, c.season) << c.path;
        EXPECT_EQ(d.episode, c.episode) << c.path;
        EXPECT_EQ(d.title, c.title) << c.path;
    }
}

TEST(Names, NotEpisodes) {
    // A year after " - " and "Episode 4" with a year are movies.
    auto d = from_name("Blade Runner - 2049.mkv");
    EXPECT_EQ(d.kind, Described::Kind::Movie);
    d = from_name("Star Wars Episode 4 - A New Hope (1977).mkv");
    EXPECT_EQ(d.kind, Described::Kind::Movie);
    EXPECT_EQ(d.year, 1977);
    // A special's prime ("- 25'") or a film in a show's folder stays a movie here;
    // video::Library::attach() makes it a special.
    d = from_name("Eva/[Anime Time] Revival Of Evangelion/[Anime Time] Neon Genesis Evangelion - 25'.mkv");
    EXPECT_EQ(d.kind, Described::Kind::Movie);
    EXPECT_EQ(d.title, "Neon Genesis Evangelion - 25'");  // no [group] tag
    d = from_name("DBZ/2a. Movies (1989-2018)/Dragon Ball Z - Movie 01 - Dead Zone (1989 BluRay - 1080p TRI AUDIO).mkv");
    EXPECT_EQ(d.title, "Dragon Ball Z - Movie 01 - Dead Zone");
    EXPECT_EQ(d.year, 1989);
}

TEST(Names, MovieYearFromItsFolder) {
    auto d = from_name("Nekromantik.2.1991.1080p.BluRay.x264-CREEPSHOW/creep-nekromantik21080.mkv");
    EXPECT_EQ(d.title, "Nekromantik 2");
    EXPECT_EQ(d.year, 1991);
    d = from_name("Interstellar/Interstellar.mkv");
    EXPECT_EQ(d.title, "Interstellar");
    EXPECT_EQ(d.year, 0);
}

TEST(Names, ExtrasAndSamples) {
    const fs::path root = "/lib/Shows";
    auto d = from_name(root / "Breaking Bad (2008) Season 1-5/Featurettes/Season 2/Deleted Scenes/Bingo.mkv", root);
    EXPECT_EQ(d.kind, Described::Kind::Extra);
    EXPECT_EQ(d.title, "Deleted Scenes - Bingo");
    EXPECT_EQ(d.season, 2);
    EXPECT_EQ(d.show, "Breaking Bad");  // a guess from the folder above the extras
    EXPECT_EQ(d.year, 2008);

    d = from_name(root / "Breaking Bad (2008)/Featurettes/Season 5/Featurettes 5.1/Chicks 'N' Guns.mkv", root);
    EXPECT_EQ(d.title, "Chicks 'N' Guns");  // "Featurettes 5.1" says nothing new
    EXPECT_EQ(d.season, 5);

    d = from_name(root / "DRAGON BALL Z (1989-2019)/1b. Extras/Dragon Ball Z (Series Extras) - Coming to America (1080p BluRay).mp4", root);
    EXPECT_EQ(d.kind, Described::Kind::Extra);
    EXPECT_EQ(d.title, "Dragon Ball Z (Series Extras) - Coming to America");

    d = from_name(root / "Amelie (2001) (1080p BluRay)/Featurettes/Trailer.mkv", root);
    EXPECT_EQ(d.show, "Amelie");
    EXPECT_EQ(d.year, 2001);

    // The top folder never counts: a show called "Extras", or a root folder named like one.
    d = from_name(root / "Extras (2005)/Season 1/Extras S01E01.mkv", root);
    EXPECT_EQ(d.kind, Described::Kind::Episode);
    EXPECT_EQ(from_name("/lib/Featurettes/Arrival (2016)/Arrival (2016).mkv", "/lib/Featurettes").kind, Described::Kind::Movie);

    for (const char* sample : {"Drive.2011/Sample/drive.2011.sample.mkv", "Rififi.1955/sample.mkv",
                               "Revenge.2017/sample-Revenge.2017.1080p.mkv", "Movie/Movie-sample.mkv"}) {
        EXPECT_EQ(from_name(sample).kind, Described::Kind::Sample) << sample;
    }
    EXPECT_NE(from_name("Samples of Life (2010)/Samples of Life (2010).mkv").kind, Described::Kind::Sample);
}

TEST(Names, LooseSeasonFoldersAndShowNames) {
    EXPECT_EQ(season_of_folder("Season 1 (1999-2000)"), 1);
    EXPECT_EQ(season_of_folder("Succession (2018) S03"), 3);
    EXPECT_EQ(season_of_folder("Kevin Can Fuck Himself (2021) S01 (1080p BluRay x265)"), 1);
    EXPECT_EQ(season_of_folder("Specials (1995-2014)"), 0);
    EXPECT_EQ(season_of_folder("Better Call Saul (2015) Season 1-6 S01-S06 (1080p BluRay)"), -1);  // a range: the show
    EXPECT_EQ(season_of_folder("Mr Robot (2015) Complete Series S01-S04"), -1);
    EXPECT_EQ(season_of_folder("Toy Story 3 (2010)"), -1);

    int year = 0;
    EXPECT_EQ(show_name("Breaking Bad (2008) Season 1-5 S01-S05 (1080p BluRay x265 HEVC 10bit AAC 5.1 Silence)", &year), "Breaking Bad");
    EXPECT_EQ(year, 2008);
    EXPECT_EQ(show_name("House of Cards (US) (2013) Season 1-6"), "House of Cards (US)");
    EXPECT_EQ(show_name("[Anime Time] Neon Genesis Evangelion Complete Series [BD] [Dual Audio]"), "Neon Genesis Evangelion");
    EXPECT_EQ(show_name("Dragon Ball The Complete Series (1995-2003)"), "Dragon Ball");
    EXPECT_EQ(show_name("Odd Taxi S01 1080p Dual Audio BDRip"), "Odd Taxi");
    EXPECT_EQ(show_name("Better Call Saul (2015) - "), "Better Call Saul");
    EXPECT_EQ(show_name("Season 1"), "");
    EXPECT_EQ(show_name("Specials"), "");

    EXPECT_TRUE(is_extras_folder("Featurettes"));
    EXPECT_TRUE(is_extras_folder("1b. Extras"));
    EXPECT_TRUE(is_extras_folder("Behind The Scenes"));
    EXPECT_TRUE(is_extras_folder("Other"));
    EXPECT_FALSE(is_extras_folder("OTHER Cartoons That You'd Like"));
    EXPECT_FALSE(is_extras_folder("Taste of Cherry (1997) Criterion + Extras"));
    const std::vector<std::string> top_first{"Extras (2005)", "Featurettes", "Deleted Scenes"};
    EXPECT_EQ(extras_folder(top_first), 1u);
}
