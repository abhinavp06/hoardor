#include <hoardor/file/scanner.hpp>

#include "file/file_time.hpp"
#include "support/temp_dir.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <set>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace fs = std::filesystem;
using hoardor::file::FileKind;
using hoardor::file::ScanError;
using hoardor::file::ScannedFile;
using hoardor::file::Scanner;
using hoardor::file::Settings;
using hoardor::test::TempDir;
using hoardor::test::u8path;

namespace {

struct Collected {
    std::vector<ScannedFile> files;
    std::vector<ScanError> errors;

    std::set<std::string> paths() const {
        std::set<std::string> out;
        for (const auto& f : files) out.insert(f.relative_path);
        return out;
    }
};

Collected drain(Scanner& scanner) {
    Collected c;
    while (auto result = scanner.next()) {
        if (*result) c.files.push_back(**result);
        else c.errors.push_back(result->error());
    }
    return c;
}

Collected scan(const fs::path& root, const Settings& settings = Settings::defaults()) {
    auto scanner = Scanner::open(root, settings);
    EXPECT_TRUE(scanner.has_value()) << scanner.error().message();
    return drain(*scanner);
}

}

TEST(ScannerOpen, FailsWhenRootIsMissing) {
    TempDir dir;
    const auto scanner = Scanner::open(dir.path() / "nope", Settings::defaults());
    ASSERT_FALSE(scanner.has_value());
    EXPECT_EQ(scanner.error(), std::errc::no_such_file_or_directory);
}

TEST(ScannerOpen, FailsWhenRootIsAFile) {
    TempDir dir;
    const auto file = dir.write("song.mp3");
    const auto scanner = Scanner::open(file, Settings::defaults());
    ASSERT_FALSE(scanner.has_value());
    EXPECT_EQ(scanner.error(), std::errc::not_a_directory);
}

TEST(Scanner, EmptyRootEndsImmediately) {
    TempDir dir;
    auto scanner = Scanner::open(dir.path(), Settings::defaults());
    ASSERT_TRUE(scanner.has_value());
    EXPECT_FALSE(scanner->next().has_value());
    EXPECT_FALSE(scanner->next().has_value()) << "stays finished";
}

TEST(Scanner, EmitsNestedMediaWithRelativeForwardSlashPaths) {
    TempDir dir;
    dir.write("Artist/Album/01 Intro.flac");
    dir.write("Artist/Album/cover.jpg");
    dir.write("top.mp3");
    const Collected c = scan(dir.path());
    EXPECT_TRUE(c.errors.empty());
    EXPECT_EQ(c.paths(), (std::set<std::string>{"Artist/Album/01 Intro.flac", "Artist/Album/cover.jpg", "top.mp3"}));
}

TEST(Scanner, SkipsDirectoriesAndNonMedia) {
    TempDir dir;
    dir.write("README");
    dir.write("notes.xyz");
    dir.write("film.MKV");
    fs::create_directories(dir.path() / "empty.mp3");  // a directory with a media-looking name
    const Collected c = scan(dir.path());
    EXPECT_EQ(c.paths(), (std::set<std::string>{"film.MKV"}));
}

TEST(Scanner, ClassifiesKinds) {
    TempDir dir;
    dir.write("a.FLAC");
    dir.write("b.mkv");
    dir.write("c.epub");
    dir.write("d.png");
    dir.write("e.srt");
    std::map<std::string, FileKind> kinds;
    for (const auto& f : scan(dir.path()).files) kinds[f.relative_path] = f.kind;
    EXPECT_EQ(kinds["a.FLAC"], FileKind::Audio);
    EXPECT_EQ(kinds["b.mkv"], FileKind::Video);
    EXPECT_EQ(kinds["c.epub"], FileKind::Text);
    EXPECT_EQ(kinds["d.png"], FileKind::Image);
    EXPECT_EQ(kinds["e.srt"], FileKind::Subtitle);
}

TEST(Scanner, ReportsExactSizeAndMtime) {
    TempDir dir;
    const auto file = dir.write("song.mp3", 12345);
    const std::int64_t mtime_ns = 1'700'000'000'123'456'789;  // 2023-11-14, with nanoseconds
    fs::last_write_time(file, hoardor::file::detail::from_unix_ns(mtime_ns));
    const Collected c = scan(dir.path());
    ASSERT_EQ(c.files.size(), 1u);
    EXPECT_EQ(c.files[0].size, 12345u);
    // ext4 and NTFS keep at least 100 ns precision; ext4 keeps the exact value.
    EXPECT_NEAR(static_cast<double>(c.files[0].mtime_ns), static_cast<double>(mtime_ns), 100.0);
}

TEST(Scanner, MtimeIsStableAcrossScans) {
    TempDir dir;
    dir.write("song.mp3");
    EXPECT_EQ(scan(dir.path()).files.at(0).mtime_ns, scan(dir.path()).files.at(0).mtime_ns);
}

TEST(Scanner, RootWithTrailingSeparatorGivesSameRelativePaths) {
    TempDir dir;
    dir.write("a/song.mp3");
    const Collected c = scan(dir.path() / "");
    EXPECT_EQ(c.paths(), (std::set<std::string>{"a/song.mp3"}));
}

TEST(Scanner, NonAsciiNamesAreUtf8AndOpenTheSameFile) {
    TempDir dir;
    const std::vector<std::string> names = {
        "\xE6\x97\xA5\xE6\x9C\xAC/\xE6\xAD\x8C.flac",  // 日本/歌.flac
        "\xF0\x9F\x8E\xB5.mp3",                        // 🎵.mp3
        "caf\xC3\xA9.mp3",                             // café (NFC)
        "cafe\xCC\x81.ogg",                            // café (NFD, as macOS writes it)
    };
    for (const auto& n : names) dir.write(u8path(n));
    const Collected c = scan(dir.path());
    EXPECT_TRUE(c.errors.empty());
    EXPECT_EQ(c.paths(), std::set<std::string>(names.begin(), names.end())) << "bytes kept, never normalized";
    for (const auto& f : c.files) EXPECT_TRUE(fs::exists(dir.path() / u8path(f.relative_path))) << f.relative_path;
}

TEST(Scanner, DeepNestingAndLongPaths) {
    TempDir dir;
    fs::path deep;
    for (int i = 0; i < 40; ++i) deep /= "folder_" + std::to_string(i);
    dir.write(deep / "track.mp3");
    const Collected c = scan(dir.path());
    ASSERT_EQ(c.files.size(), 1u);
    EXPECT_GT(c.files[0].relative_path.size(), 260u);
}

TEST(Scanner, IgnoredNamesAndPrefixesAreSkippedAndNotDescended) {
    TempDir dir;
    const Settings settings = Settings::defaults();
    for (const auto& name : settings.ignored_names) {
        dir.write(fs::path(name) / "inside.mp3");  // as a directory
    }
    dir.write("._song.mp3");
    dir.write(".Trash-1000/files/old.mp3");
    dir.write("@EADIR/thumb.jpg");  // case-insensitive match
    dir.write("keep.mp3");
    auto scanner = Scanner::open(dir.path(), settings);
    ASSERT_TRUE(scanner.has_value());
    const Collected c = drain(*scanner);
    EXPECT_EQ(c.paths(), (std::set<std::string>{"keep.mp3"}));
    EXPECT_EQ(scanner->progress().directories_visited, 1u) << "only the root was opened";
}

TEST(Scanner, IgnoredFileNamesAreSkipped) {
    TempDir dir;
    Settings settings = Settings::defaults();
    settings.ignored_names.push_back("skip-me.mp3");
    dir.write("skip-me.mp3");
    dir.write("SKIP-ME.MP3.mp3");
    EXPECT_EQ(scan(dir.path(), settings).paths(), (std::set<std::string>{"SKIP-ME.MP3.mp3"}));
}

TEST(Scanner, CustomSettingsAreRespected) {
    TempDir dir;
    dir.write("a.mp3");
    dir.write("b.nfo");
    Settings settings;
    settings.extension_kinds = {{".NFO", FileKind::Text}};
    EXPECT_EQ(scan(dir.path(), settings).paths(), (std::set<std::string>{"b.nfo"}));
    EXPECT_TRUE(scan(dir.path(), Settings{}).files.empty()) << "empty settings: nothing is media";
}

TEST(Scanner, SymlinksAreNeitherEmittedNorFollowed) {
    TempDir dir;
    const auto target = dir.write("real/song.mp3");
    std::error_code ec;
    fs::create_symlink(target, dir.path() / "link.mp3", ec);
    if (ec) GTEST_SKIP() << "cannot create symlinks here: " << ec.message();
    fs::create_directory_symlink(dir.path() / "real", dir.path() / "linkdir", ec);
    ASSERT_FALSE(ec);
    fs::create_directory_symlink(dir.path(), dir.path() / "real" / "loop", ec);  // a cycle
    ASSERT_FALSE(ec);
    EXPECT_EQ(scan(dir.path()).paths(), (std::set<std::string>{"real/song.mp3"}));
}

TEST(Scanner, UnreadableSubdirectoryIsReportedAndScanContinues) {
    TempDir dir;
    dir.write("locked/secret.mp3");
    dir.write("open/song.mp3");
    const auto locked = dir.path() / "locked";
    fs::permissions(locked, fs::perms::none);
    struct Restore {
        fs::path p;
        ~Restore() { fs::permissions(p, fs::perms::owner_all); }
    } restore{locked};
    std::error_code ec;
    fs::directory_iterator probe(locked, ec);
    if (!ec) GTEST_SKIP() << "running with privileges that ignore permissions (e.g. root)";

    const Collected c = scan(dir.path());
    EXPECT_EQ(c.paths(), (std::set<std::string>{"open/song.mp3"}));
    ASSERT_EQ(c.errors.size(), 1u);
    EXPECT_EQ(c.errors[0].relative_path, "locked");
    EXPECT_TRUE(c.errors[0].is_directory);
    EXPECT_EQ(c.errors[0].code, std::errc::permission_denied);
}

TEST(Scanner, RootRemovedMidScanReportsRootLostOnceAndLast) {
    TempDir dir;
    for (int d = 0; d < 5; ++d) {
        for (int f = 0; f < 60; ++f) dir.write("d" + std::to_string(d) + "/t" + std::to_string(f) + ".mp3");
    }
    auto scanner = Scanner::open(dir.path(), Settings::defaults());
    ASSERT_TRUE(scanner.has_value());
    auto first = scanner->next();
    ASSERT_TRUE(first.has_value() && first->has_value());

    fs::remove_all(dir.path());  // the drive is unplugged

    std::vector<hoardor::file::ScanResult> rest;
    while (auto r = scanner->next()) rest.push_back(*r);
    ASSERT_FALSE(rest.empty());
    ASSERT_FALSE(rest.back().has_value()) << "the last item is the root error";
    EXPECT_TRUE(rest.back().error().relative_path.empty());
    EXPECT_TRUE(rest.back().error().is_directory);
    const auto root_errors = std::count_if(rest.begin(), rest.end(), [](const auto& r) {
        return !r.has_value() && r.error().relative_path.empty();
    });
    EXPECT_EQ(root_errors, 1);
    EXPECT_FALSE(scanner->next().has_value());
}

TEST(Scanner, FilesVanishingMidScanAreSilentlySkipped) {
    TempDir dir;
    for (int f = 0; f < 200; ++f) dir.write("t" + std::to_string(f) + ".mp3");
    auto scanner = Scanner::open(dir.path(), Settings::defaults());
    ASSERT_TRUE(scanner.has_value());
    auto first = scanner->next();
    ASSERT_TRUE(first.has_value() && first->has_value());
    for (int f = 0; f < 200; ++f) {
        const auto p = dir.path() / ("t" + std::to_string(f) + ".mp3");
        if ((*first)->relative_path != p.filename().string()) fs::remove(p);
    }
    const Collected c = drain(*scanner);
    EXPECT_TRUE(c.errors.empty());
}

TEST(Scanner, DestroyedMidScanIsSafe) {
    TempDir dir;
    for (int f = 0; f < 10; ++f) dir.write("a/t" + std::to_string(f) + ".mp3");
    {
        auto scanner = Scanner::open(dir.path(), Settings::defaults());
        ASSERT_TRUE(scanner.has_value());
        ASSERT_TRUE(scanner->next().has_value());
    }
    SUCCEED();
}

TEST(Scanner, ProgressCountersMatchTheTree) {
    TempDir dir;
    dir.write("a/1.mp3");
    dir.write("a/b/2.mp3");
    dir.write("c/3.txt");
    dir.write("c/skip.xyz");
    auto scanner = Scanner::open(dir.path(), Settings::defaults());
    ASSERT_TRUE(scanner.has_value());
    drain(*scanner);
    EXPECT_EQ(scanner->progress().directories_visited, 4u);  // root, a, a/b, c
    EXPECT_EQ(scanner->progress().files_emitted, 3u);
    EXPECT_EQ(scanner->progress().errors, 0u);
}

TEST(Scanner, NamesDifferingOnlyInCaseAreBothEmitted) {
    TempDir dir;
    dir.write("song.mp3");
    dir.write("SONG.mp3");
    if (std::distance(fs::directory_iterator(dir.path()), fs::directory_iterator{}) != 2) {
        GTEST_SKIP() << "case-insensitive file system";
    }
    EXPECT_EQ(scan(dir.path()).paths(), (std::set<std::string>{"song.mp3", "SONG.mp3"}));
}

#ifndef _WIN32
TEST(Scanner, FifoIsNotEmitted) {
    TempDir dir;
    ASSERT_EQ(::mkfifo((dir.path() / "pipe.mp3").c_str(), 0600), 0);
    dir.write("song.mp3");
    EXPECT_EQ(scan(dir.path()).paths(), (std::set<std::string>{"song.mp3"}));
}

TEST(Scanner, InvalidUtf8NamesAreReportedNotEmitted) {
    TempDir dir;
    dir.write(fs::path(std::string("bad\xE9.mp3")));           // Latin-1 é in a file name
    dir.write(fs::path(std::string("dir\xFF")) / "in.mp3");    // and in a directory name
    dir.write("good.mp3");
    const Collected c = scan(dir.path());
    EXPECT_EQ(c.paths(), (std::set<std::string>{"good.mp3"}));
    ASSERT_EQ(c.errors.size(), 2u);
    for (const auto& e : c.errors) {
        EXPECT_EQ(e.code, std::errc::illegal_byte_sequence);
        EXPECT_NE(e.relative_path.find("\xEF\xBF\xBD"), std::string::npos) << "repaired for display";
    }
}
#endif
