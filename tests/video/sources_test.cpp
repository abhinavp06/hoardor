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
    EXPECT_EQ(d.title, "Ghosts 1080p");

    d = from_name("tv/The Bear - s02e10 - Omelette.mkv");
    EXPECT_EQ(d.kind, Described::Kind::Episode);
    EXPECT_EQ(d.show, "The Bear");
    EXPECT_EQ(d.season, 2);
    EXPECT_EQ(d.episode, 10);
    EXPECT_EQ(d.title, "Omelette");

    d = from_name("Fargo/fargo.3x07.mkv");
    EXPECT_EQ(d.season, 3);
    EXPECT_EQ(d.episode, 7);
    EXPECT_EQ(d.show, "fargo");

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
}
