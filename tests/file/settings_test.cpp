#include <algorithm>
#include <hoardor/file/settings.hpp>

#include <gtest/gtest.h>

using hoardor::file::FileKind;
using hoardor::file::Settings;
using hoardor::file::kind_of;

TEST(SettingsDefaults, CoverEveryKind) {
    const Settings s = Settings::defaults();
    EXPECT_EQ(kind_of("song.mp3", s), FileKind::Audio);
    EXPECT_EQ(kind_of("book.m4b", s), FileKind::Audio);
    EXPECT_EQ(kind_of("film.mkv", s), FileKind::Video);
    EXPECT_EQ(kind_of("show.m2ts", s), FileKind::Video);
    EXPECT_EQ(kind_of("novel.epub", s), FileKind::Text);
    EXPECT_EQ(kind_of("cover.jpg", s), FileKind::Image);
    EXPECT_EQ(kind_of("film.srt", s), FileKind::Subtitle);
}

TEST(SettingsDefaults, IgnoreListsHoldExternalDriveLitter) {
    const Settings s = Settings::defaults();
    const auto has = [](const std::vector<std::string>& v, const std::string& x) {
        return std::find(v.begin(), v.end(), x) != v.end();
    };
    for (const char* name : {"$RECYCLE.BIN", "System Volume Information", "Thumbs.db", "desktop.ini", "FOUND.000",
                             ".DS_Store", ".Trashes", ".Spotlight-V100", ".fseventsd", ".TemporaryItems",
                             ".DocumentRevisions-V100", "lost+found", "@eaDir", "#recycle", "#snapshot",
                             ".snapshot", ".hoardor-root"}) {
        EXPECT_TRUE(has(s.ignored_names, name)) << name;
    }
    EXPECT_TRUE(has(s.ignored_prefixes, "._"));
    EXPECT_TRUE(has(s.ignored_prefixes, ".Trash-"));
}

TEST(KindOf, IgnoresExtensionCase) {
    const Settings s = Settings::defaults();
    EXPECT_EQ(kind_of("SONG.FLAC", s), FileKind::Audio);
    EXPECT_EQ(kind_of("Song.FlAc", s), FileKind::Audio);
}

TEST(KindOf, AcceptsUserKeysWithDotOrUppercase) {
    Settings s;
    s.extension_kinds = {{".DSF", FileKind::Audio}, {"Mka", FileKind::Audio}};
    EXPECT_EQ(kind_of("a.dsf", s), FileKind::Audio);
    EXPECT_EQ(kind_of("a.mka", s), FileKind::Audio);
}

TEST(KindOf, RespectsEditedMap) {
    Settings s = Settings::defaults();
    s.extension_kinds.erase("ts");                    // the user's .ts files are TypeScript
    s.extension_kinds["txt"] = FileKind::Subtitle;    // remapped
    s.extension_kinds["nfo"] = FileKind::Text;        // added
    EXPECT_EQ(kind_of("clip.ts", s), std::nullopt);
    EXPECT_EQ(kind_of("notes.txt", s), FileKind::Subtitle);
    EXPECT_EQ(kind_of("movie.nfo", s), FileKind::Text);
}

TEST(KindOf, NoExtensionIsNotMedia) {
    EXPECT_EQ(kind_of("README", Settings::defaults()), std::nullopt);
}

TEST(KindOf, DotOnlyNameIsNotMedia) {
    // std::filesystem treats ".mp3" as a stem with no extension.
    EXPECT_EQ(kind_of(".mp3", Settings::defaults()), std::nullopt);
}

TEST(KindOf, MultipleDotsUseTheLastExtension) {
    const Settings s = Settings::defaults();
    EXPECT_EQ(kind_of("a.live.flac", s), FileKind::Audio);
    EXPECT_EQ(kind_of("a.flac.txt", s), FileKind::Text);
    EXPECT_EQ(kind_of("trailing.", s), std::nullopt);
}

TEST(KindOf, EmptySettingsMeanNothingIsMedia) {
    EXPECT_EQ(kind_of("song.mp3", Settings{}), std::nullopt);
}

TEST(FileKindNames, RoundTrip) {
    for (FileKind k : {FileKind::Audio, FileKind::Video, FileKind::Text, FileKind::Image, FileKind::Subtitle}) {
        EXPECT_EQ(hoardor::file::file_kind_from_string(hoardor::file::to_string(k)), k);
    }
    EXPECT_EQ(hoardor::file::file_kind_from_string("AUDIO"), FileKind::Audio);
    EXPECT_EQ(hoardor::file::file_kind_from_string("podcast"), std::nullopt);
}

TEST(Validate, DefaultsAreValid) {
    EXPECT_TRUE(hoardor::file::validate(Settings::defaults()).empty());
}

TEST(Validate, FlagsOutOfRangeNumbers) {
    Settings s = Settings::defaults();
    s.mass_removal_threshold_percent = 150;
    s.batch_max_rows = 0;
    s.settle_window_seconds = -1;
    s.parallel_devices = 0;
    const auto problems = hoardor::file::validate(s);
    ASSERT_EQ(problems.size(), 4u);
    EXPECT_NE(problems[0].find("between 0 and 86400"), std::string::npos) << problems[0];
}

TEST(Validate, FlagsValuesThatWouldCorruptStorage) {
    Settings s = Settings::defaults();
    s.extension_kinds["a=b"] = FileKind::Audio;
    s.extension_kinds["  "] = FileKind::Audio;
    s.ignored_names.push_back("two\nlines");
    s.ignored_prefixes.push_back("");
    EXPECT_EQ(hoardor::file::validate(s).size(), 4u);
}
