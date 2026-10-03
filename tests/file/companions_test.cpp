#include <hoardor/file/library.hpp>

#include "support/library_fixture.hpp"

#include <gtest/gtest.h>

#include <set>

namespace fs = std::filesystem;
using namespace hoardor::file;
using hoardor::test::LibraryTest;

namespace {

class CompanionsTest : public LibraryTest {
protected:
    Root add(std::string_view category_name, const fs::path& folder) {
        fs::create_directories(folder);
        auto root = library->add_root(category(category_name), folder, {}, false);
        EXPECT_TRUE(root.has_value()) << root.error().message;
        const auto report = library->sync_root(root->id);
        EXPECT_EQ(report.outcome, RootSyncOutcome::Synced) << report.message;
        return *root;
    }

    EntryId id_of(RootId root, std::string_view relative) {
        for (const auto& e : all_entries(root)) {
            if (e.relative_path == relative) return e.id;
        }
        ADD_FAILURE() << "no entry " << relative;
        return 0;
    }

    std::set<std::string> names(const std::vector<Entry>& entries) {
        std::set<std::string> out;
        for (const auto& e : entries) out.insert(e.relative_path);
        return out;
    }
};

}

TEST_F(CompanionsTest, ImagesNextToATrackButNotInOtherFolders) {
    dir.write("music/Artist/Album/01 One.flac", 1);
    dir.write("music/Artist/Album/02 Two.flac", 1);
    dir.write("music/Artist/Album/cover.jpg", 1);
    dir.write("music/Artist/Album/Scans/back.jpg", 1);  // a subfolder: not a companion of the tracks
    dir.write("music/Artist/artist.jpg", 1);             // the parent: only with parent_levels
    const Root root = add("Music", dir.path() / "music");
    const EntryId track = id_of(root.id, "Artist/Album/01 One.flac");

    EXPECT_EQ(names(library->companions(track).value()), (std::set<std::string>{"Artist/Album/cover.jpg"}));
    EXPECT_EQ(names(library->companions(track, 1).value()),
              (std::set<std::string>{"Artist/Album/cover.jpg", "Artist/artist.jpg"}));
}

TEST_F(CompanionsTest, AVideosNfoSubtitlesAndTheShowsFilesTwoLevelsUp) {
    dir.write("shows/Dark/tvshow.nfo", 1);
    dir.write("shows/Dark/poster.jpg", 1);
    dir.write("shows/Dark/Season 1/Dark S01E01.mkv", 1);
    dir.write("shows/Dark/Season 1/Dark S01E01.nfo", 1);
    dir.write("shows/Dark/Season 1/Dark S01E01.en.srt", 1);
    dir.write("shows/Dark/Season 1/Dark S01E02.mkv", 1);  // a video: never a companion
    dir.write("shows/Other/poster.jpg", 1);                 // another show
    const Root root = add("Shows", dir.path() / "shows");
    const EntryId episode = id_of(root.id, "Dark/Season 1/Dark S01E01.mkv");

    const auto nearest = library->companions(episode, 2).value();
    EXPECT_EQ(names(nearest), (std::set<std::string>{"Dark/Season 1/Dark S01E01.nfo", "Dark/Season 1/Dark S01E01.en.srt",
                                                      "Dark/tvshow.nfo", "Dark/poster.jpg"}));
    // Nearest folder first.
    EXPECT_EQ(nearest.front().relative_path.rfind("Dark/Season 1/", 0), 0u);
}

TEST_F(CompanionsTest, FilesDirectlyInTheRoot) {
    dir.write("films/Arrival.mkv", 1);
    dir.write("films/Arrival.nfo", 1);
    dir.write("films/sub/Other.nfo", 1);
    const Root root = add("Movies", dir.path() / "films");
    EXPECT_EQ(names(library->companions(id_of(root.id, "Arrival.mkv"), 2).value()), (std::set<std::string>{"Arrival.nfo"}));
}

TEST_F(CompanionsTest, CaseInsensitiveRootsAndTheLimit) {
    dir.write("music/Album/Track.flac", 1);
    for (int i = 0; i < 5; ++i) dir.write("music/Album/scan" + std::to_string(i) + ".jpg", 1);
    const Root root = add("Music", dir.path() / "music");
    ASSERT_TRUE(library->set_root_case_sensitive(root.id, false));
    const EntryId track = id_of(root.id, "Album/Track.flac");
    EXPECT_EQ(library->companions(track).value().size(), 5u);
    EXPECT_EQ(library->companions(track, 0, 2).value().size(), 2u);
}

TEST_F(CompanionsTest, UnknownEntry) {
    const auto r = library->companions(4242);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ErrorCode::NotFound);
}

TEST_F(CompanionsTest, OneEntryById) {
    dir.write("music/a.flac", 7);
    const Root root = add("Music", dir.path() / "music");
    const EntryId id = id_of(root.id, "a.flac");
    const auto e = library->entry(id);
    ASSERT_TRUE(e.has_value());
    EXPECT_EQ(e->relative_path, "a.flac");
    EXPECT_EQ(e->size, 7u);
    EXPECT_EQ(e->root_id, root.id);
    EXPECT_EQ(library->entry(id + 1000).error().code, ErrorCode::NotFound);
}

TEST_F(CompanionsTest, PrefixesFindAMoviesSidecarsInAFlatFolderOfHundreds) {
    // 300 movies side by side, each with its .nfo, poster, and subtitles: 1,200 files.
    for (int i = 0; i < 300; ++i) {
        const std::string name = "films/Movie " + std::to_string(i);
        for (const char* ext : {".mkv", ".nfo", ".jpg", ".en.srt"}) dir.write(name + ext, 1);
    }
    dir.write("films/poster.jpg", 1);
    const Root root = add("Movies", dir.path() / "films");
    const EntryId movie = id_of(root.id, "Movie 250.mkv");

    // Without prefixes the first 200 files fill the list; Movie 250's are never reached.
    EXPECT_EQ(library->companions(movie, 2).value().size(), 200u);
    EXPECT_FALSE(names(library->companions(movie, 2).value()).contains("Movie 250.nfo"));

    // "Movie 25." must not match "Movie 250.nfo".
    const std::vector<std::string> prefixes{"Movie 250.", "Movie 250-poster.", "poster."};
    EXPECT_EQ(names(library->companions(movie, 2, 200, prefixes).value()),
              (std::set<std::string>{"Movie 250.nfo", "Movie 250.jpg", "Movie 250.en.srt", "poster.jpg"}));
    const std::vector<std::string> other{"Movie 25."};
    EXPECT_EQ(names(library->companions(id_of(root.id, "Movie 25.mkv"), 0, 200, other).value()),
              (std::set<std::string>{"Movie 25.nfo", "Movie 25.jpg", "Movie 25.en.srt"}));
}

TEST_F(CompanionsTest, PrefixesNearestFolderFirstAndEachFileOnce) {
    dir.write("shows/Dark/poster.jpg", 1);
    dir.write("shows/Dark/tvshow.nfo", 1);
    dir.write("shows/Dark/Season 1/Dark S01E01.mkv", 1);
    dir.write("shows/Dark/Season 1/poster.jpg", 1);
    for (int i = 0; i < 250; ++i) dir.write("shows/Dark/Season 1/Dark S01E01." + std::to_string(i) + ".srt", 1);
    const Root root = add("Shows", dir.path() / "shows");
    const EntryId episode = id_of(root.id, "Dark/Season 1/Dark S01E01.mkv");

    // 250 subtitles no longer hide the show's poster one folder up.
    const std::vector<std::string> prefixes{"poster.", "poster", "tvshow."};  // "poster" and "poster." overlap
    const auto found = library->companions(episode, 2, 200, prefixes).value();
    ASSERT_EQ(found.size(), 3u);
    EXPECT_EQ(found[0].relative_path, "Dark/Season 1/poster.jpg");
    EXPECT_EQ(names(found), (std::set<std::string>{"Dark/Season 1/poster.jpg", "Dark/poster.jpg", "Dark/tvshow.nfo"}));
}

TEST_F(CompanionsTest, PrefixesMatchCommonSpellings) {
    dir.write("films/Heat/Heat.mkv", 1);
    dir.write("films/Heat/Poster.jpg", 1);
    dir.write("films/Heat/FOLDER.JPG", 1);
    dir.write("films/Heat/heat.nfo", 1);   // a stem in another case: only on a case-insensitive root
    dir.write("films/Heat/pOsTeR.png", 1);  // no common spelling: only on a case-insensitive root
    const Root root = add("Movies", dir.path() / "films");
    const std::vector<std::string> prefixes{"Heat.", "poster.", "folder."};
    const EntryId movie = id_of(root.id, "Heat/Heat.mkv");

    if (root.case_sensitive) {
        EXPECT_EQ(names(library->companions(movie, 0, 200, prefixes).value()),
                  (std::set<std::string>{"Heat/Poster.jpg", "Heat/FOLDER.JPG", "Heat/heat.nfo"}));  // heat. = lowercase "Heat."
    }
    ASSERT_TRUE(library->set_root_case_sensitive(root.id, false));
    EXPECT_EQ(names(library->companions(movie, 0, 200, prefixes).value()),
              (std::set<std::string>{"Heat/Poster.jpg", "Heat/FOLDER.JPG", "Heat/heat.nfo", "Heat/pOsTeR.png"}));
}
