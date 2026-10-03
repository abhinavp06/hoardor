#include <hoardor/video/video.hpp>

#include "support/temp_dir.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>
#include <iterator>

namespace fs = std::filesystem;
using namespace hoardor;

namespace {

void set_env(const char* name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    if (value.empty()) unsetenv(name);
    else setenv(name, value.c_str(), 1);
#endif
}

std::string env(const char* name) {
    const char* v = std::getenv(name);
    return v ? v : "";
}

std::string contents(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// A fake Plex data folder: the three tables hoardor reads (only the columns it uses) and the
// bundles' image files (features/posters.md §3).
class PlexPostersTest : public ::testing::Test {
protected:
    void SetUp() override {
        plex = dir.path() / "Plex Media Server";
        fs::create_directories(plex / "Plug-in Support" / "Databases");
        auto opened = db::Database::open(plex / "Plug-in Support" / "Databases" / "com.plexapp.plugins.library.db");
        ASSERT_TRUE(opened.has_value());
        database.emplace(std::move(*opened));
        ASSERT_TRUE(database->exec(R"sql(
CREATE TABLE metadata_items (id INTEGER PRIMARY KEY, parent_id INTEGER, metadata_type INTEGER, hash TEXT, user_thumb_url TEXT, title TEXT);
CREATE TABLE media_items (id INTEGER PRIMARY KEY, metadata_item_id INTEGER);
CREATE TABLE media_parts (id INTEGER PRIMARY KEY, media_item_id INTEGER, file TEXT);
)sql"));
    }

    // A metadata item; returns its id.
    std::int64_t item(int type, const std::string& hash, const std::string& thumb, std::int64_t parent = 0) {
        auto st = database->prepare("INSERT INTO metadata_items (parent_id, metadata_type, hash, user_thumb_url) VALUES (?, ?, ?, ?)");
        st->bind(1, parent).bind(2, type).bind(3, hash).bind(4, thumb);
        EXPECT_TRUE(st->run());
        return database->last_insert_id();
    }

    void part(std::int64_t metadata, const std::string& file) {
        auto media = database->prepare("INSERT INTO media_items (metadata_item_id) VALUES (?)");
        media->bind(1, metadata);
        EXPECT_TRUE(media->run());
        auto st = database->prepare("INSERT INTO media_parts (media_item_id, file) VALUES (?, ?)");
        st->bind(1, database->last_insert_id()).bind(2, file);
        EXPECT_TRUE(st->run());
    }

    // An image file inside a bundle (hash "abc…" → Metadata/<kind>/a/bc….bundle/<inside>).
    fs::path image(bool show, const std::string& hash, const fs::path& inside) {
        const fs::path p = plex / "Metadata" / (show ? "TV Shows" : "Movies") / hash.substr(0, 1) / (hash.substr(1) + ".bundle") / inside;
        fs::create_directories(p.parent_path());
        std::ofstream(p, std::ios::binary) << "jpeg";
        return p;
    }

    video::PlexPosters open() {
        database.reset();  // Plex's writes are done; hoardor reads them
        auto p = video::PlexPosters::open(plex);
        EXPECT_TRUE(p.has_value()) << p.error().message;
        return std::move(*p);
    }

    test::TempDir dir;
    fs::path plex;
    std::optional<db::Database> database;
};

}

TEST_F(PlexPostersTest, AMoviesChosenPosterEvenUnderAnotherDriveLetter) {
    const auto heat = item(1, "a1b2c3", "metadata://posters/com.plexapp.agents.imdb_9f8e");
    part(heat, "E:\\Movies\\Heat (1995)\\Heat (1995).mkv");
    const fs::path poster = image(false, "a1b2c3", "Contents/_combined/posters/com.plexapp.agents.imdb_9f8e");
    auto posters = open();
    EXPECT_EQ(posters.poster("E:/Movies/Heat (1995)/Heat (1995).mkv"), poster);
    EXPECT_EQ(posters.poster("F:/Films/Movies/Heat (1995)/heat (1995).MKV"), poster);  // another letter, other case
    EXPECT_EQ(posters.poster("E:/Movies/Arrival (2016)/Arrival.mkv"), std::nullopt);   // Plex doesn't know it
}

TEST_F(PlexPostersTest, AnEpisodeGetsItsShowsPosterElseItsSeasons) {
    const auto lost = item(2, "5e1", "upload://posters/77aa");
    const auto lost_s1 = item(3, "5e2", "metadata://posters/season1", lost);
    part(item(4, "5e3", "", lost_s1), "\\\\nas\\tv\\Lost\\Season 1\\Lost S01E01.mkv");
    const fs::path show_poster = image(true, "5e1", "Uploads/posters/77aa");
    image(true, "5e2", "Contents/_combined/posters/season1");

    const auto dark = item(2, "d01", "http://example.com/never-fetched.jpg");  // a web address only
    const auto dark_s1 = item(3, "d02", "metadata://posters/s1", dark);
    part(item(4, "d03", "", dark_s1), "D:\\TV\\Dark\\Season 1\\Dark S01E01.mkv");
    const fs::path season_poster = image(true, "d02", "Contents/_combined/posters/s1");

    auto posters = open();
    EXPECT_EQ(posters.poster("Z:/TV/Lost/Season 1/Lost S01E01.mkv"), show_poster);  // a share in Plex, a letter here
    EXPECT_EQ(posters.poster("D:/TV/Dark/Season 1/Dark S01E01.mkv"), season_poster);
}

TEST_F(PlexPostersTest, AMissingImageFallsBackToAnotherPosterOfTheSameItem) {
    part(item(1, "f00", "metadata://posters/gone"), "E:\\Movies\\Fargo\\Fargo.mkv");
    const fs::path other = image(false, "f00", "Contents/_combined/posters/tv.plex.agents.movie_123");
    part(item(1, "e00", "media://1/2a.bundle/Contents/Thumbnails/thumb1.jpg"), "E:\\Movies\\Home\\Home.mkv");
    const fs::path media = plex / "Media" / "localhost" / "1" / "2a.bundle" / "Contents" / "Thumbnails" / "thumb1.jpg";
    fs::create_directories(media.parent_path());
    std::ofstream(media) << "jpeg";
    part(item(1, "b00", "http://example.com/x.jpg"), "E:\\Movies\\Web\\Web.mkv");  // nothing on disk
    auto posters = open();
    EXPECT_EQ(posters.poster("E:/Movies/Fargo/Fargo.mkv"), other);
    EXPECT_EQ(posters.poster("E:/Movies/Home/Home.mkv"), media);
    EXPECT_EQ(posters.poster("E:/Movies/Web/Web.mkv"), std::nullopt);
}

TEST_F(PlexPostersTest, TheSameFolderAndNameTwiceNeedsTheExactPath) {
    part(item(1, "aa1", "metadata://posters/one"), "X:\\Movies\\Dup\\Dup.mkv");
    part(item(1, "aa2", "metadata://posters/two"), "Y:\\Movies\\Dup\\Dup.mkv");
    image(false, "aa1", "Contents/_combined/posters/one");
    const fs::path two = image(false, "aa2", "Contents/_combined/posters/two");
    auto posters = open();
    EXPECT_EQ(posters.poster("Y:/Movies/Dup/Dup.mkv"), two);
    EXPECT_EQ(posters.poster("Z:/Movies/Dup/Dup.mkv"), std::nullopt);  // which one? neither
}

TEST_F(PlexPostersTest, GenericEpisodeNamesAreToldApartByTheirShow) {
    part(item(4, "", "", item(3, "", "", item(2, "10a", "metadata://posters/lost"))), "A:\\TV\\Lost\\Season 1\\01.mkv");
    part(item(4, "", "", item(3, "", "", item(2, "20a", "metadata://posters/dark"))), "A:\\TV\\Dark\\Season 1\\01.mkv");
    image(true, "10a", "Contents/_combined/posters/lost");
    const fs::path dark = image(true, "20a", "Contents/_combined/posters/dark");
    auto posters = open();
    EXPECT_EQ(posters.poster("Z:/TV/Dark/Season 1/01.mkv"), dark);
}

TEST_F(PlexPostersTest, PlexsDatabaseIsNeverWritten) {
    part(item(1, "c00", "metadata://posters/p"), "E:\\Movies\\C\\C.mkv");
    image(false, "c00", "Contents/_combined/posters/p");
    database.reset();
    const fs::path file = plex / "Plug-in Support" / "Databases" / "com.plexapp.plugins.library.db";
    const std::string before = contents(file);
    {
        auto posters = open();
        EXPECT_TRUE(posters.poster("E:/Movies/C/C.mkv").has_value());
    }
    EXPECT_EQ(contents(file), before);
}

TEST_F(PlexPostersTest, NoPlexOrAnUnexpectedDatabaseFailsToOpen) {
    EXPECT_FALSE(video::PlexPosters::open(dir.path() / "nowhere").has_value());
    ASSERT_TRUE(database->exec("DROP TABLE media_parts;"));
    database.reset();
    EXPECT_FALSE(video::PlexPosters::open(plex).has_value());
}

TEST_F(PlexPostersTest, FindsPlexsFolderInLocalAppData) {
    const std::string saved_local = env("LOCALAPPDATA"), saved_home = env("HOME");
    set_env("LOCALAPPDATA", dir.path().string());
    EXPECT_EQ(video::PlexPosters::find_folder(), plex);
    set_env("LOCALAPPDATA", (dir.path() / "empty").string());
    set_env("HOME", (dir.path() / "empty").string());
    if (!fs::exists("/var/lib/plexmediaserver")) EXPECT_EQ(video::PlexPosters::find_folder(), std::nullopt);
    set_env("LOCALAPPDATA", saved_local);
    set_env("HOME", saved_home);
}
