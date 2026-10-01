#include <hoardor/file/library.hpp>

#include "file/root_marker.hpp"
#include "support/library_fixture.hpp"

#include <gtest/gtest.h>

#include <algorithm>

namespace fs = std::filesystem;
using namespace hoardor::file;
using hoardor::test::TempDir;

using hoardor::test::LibraryTest;


// ---------------------------------------------------------------- Schema and settings

TEST_F(LibraryTest, SeedsDefaultCategories) {
    const auto categories = library->categories().value();
    ASSERT_EQ(categories.size(), 4u);
    EXPECT_EQ(categories[0].name, "Music");
    EXPECT_EQ(categories[0].kinds, (std::vector<FileKind>{FileKind::Audio, FileKind::Image}));
    EXPECT_EQ(categories[1].name, "Movies");
    EXPECT_EQ(categories[1].kinds, (std::vector<FileKind>{FileKind::Video, FileKind::Subtitle, FileKind::Image}));
    EXPECT_EQ(categories[2].name, "Shows");
    EXPECT_EQ(categories[3].name, "Books");
    EXPECT_EQ(categories[3].kinds, (std::vector<FileKind>{FileKind::Text, FileKind::Image}));
}

TEST_F(LibraryTest, OpeningAgainKeepsData) {
    ASSERT_TRUE(library->add_category("Podcasts", {FileKind::Audio}));
    auto again = Library::open(*db);
    ASSERT_TRUE(again.has_value());
    EXPECT_EQ(again->categories().value().size(), 5u);
}

TEST_F(LibraryTest, SettingsDefaultWhenNothingStored) {
    const Settings loaded = library->load_settings().value();
    const Settings defaults = Settings::defaults();
    EXPECT_EQ(loaded.extension_kinds, defaults.extension_kinds);
    EXPECT_EQ(loaded.ignored_names, defaults.ignored_names);
    EXPECT_FALSE(loaded.sync_on_startup);
    EXPECT_EQ(loaded.mass_removal_threshold_percent, 25);
}

TEST_F(LibraryTest, SettingsRoundTripEveryField) {
    Settings s = Settings::defaults();
    s.extension_kinds = {{".MKA", FileKind::Audio}, {"nfo", FileKind::Text}};
    s.ignored_names = {"Extras", "Sample"};
    s.ignored_prefixes = {};
    s.sync_on_startup = true;
    s.settle_window_seconds = 30;
    s.mass_removal_threshold_percent = 50;
    s.batch_max_rows = 100;
    s.batch_max_milliseconds = 20;
    s.progress_interval_files = 10;
    s.relocation_sample_size = 5;
    s.relocation_min_match_percent = 60;
    ASSERT_TRUE(library->save_settings(s));
    const Settings l = library->load_settings().value();
    EXPECT_EQ(l.extension_kinds, (std::unordered_map<std::string, FileKind>{{"mka", FileKind::Audio}, {"nfo", FileKind::Text}}));
    EXPECT_EQ(l.ignored_names, s.ignored_names);
    EXPECT_TRUE(l.ignored_prefixes.empty()) << "an empty list is a valid choice";
    EXPECT_TRUE(l.sync_on_startup);
    EXPECT_EQ(l.settle_window_seconds, 30);
    EXPECT_EQ(l.mass_removal_threshold_percent, 50);
    EXPECT_EQ(l.batch_max_rows, 100);
    EXPECT_EQ(l.batch_max_milliseconds, 20);
    EXPECT_EQ(l.progress_interval_files, 10);
    EXPECT_EQ(l.relocation_sample_size, 5);
    EXPECT_EQ(l.relocation_min_match_percent, 60);
}

TEST_F(LibraryTest, CorruptSettingValuesFallBackToDefaults) {
    ASSERT_TRUE(db->exec("INSERT INTO file_settings (key, value) VALUES "
                         "('batch_max_rows', 'abc'), ('mass_removal_threshold_percent', '150'), "
                         "('extension_kinds', 'mp3=audio\nbroken'), ('sync_on_startup', 'yes'), "
                         "('settle_window_seconds', '-5'), ('progress_interval_files', '0')"));
    const Settings l = library->load_settings().value();
    const Settings d = Settings::defaults();
    EXPECT_EQ(l.batch_max_rows, d.batch_max_rows);
    EXPECT_EQ(l.mass_removal_threshold_percent, d.mass_removal_threshold_percent);
    EXPECT_EQ(l.extension_kinds, d.extension_kinds) << "one bad line rejects the whole value";
    EXPECT_EQ(l.sync_on_startup, d.sync_on_startup);
    EXPECT_EQ(l.settle_window_seconds, d.settle_window_seconds);
    EXPECT_EQ(l.progress_interval_files, d.progress_interval_files);
}

TEST_F(LibraryTest, UnknownSettingKeysAreIgnored) {
    ASSERT_TRUE(db->exec("INSERT INTO file_settings (key, value) VALUES ('from_a_newer_build', '42')"));
    EXPECT_TRUE(library->load_settings().has_value());
}

// ---------------------------------------------------------------- Categories

TEST_F(LibraryTest, AddUpdateRemoveCategory) {
    auto id = library->add_category("  Podcasts ", {FileKind::Audio});
    ASSERT_TRUE(id.has_value());
    ASSERT_TRUE(library->update_category(Category{*id, "Talks", {FileKind::Audio, FileKind::Video}}));
    const auto all = library->categories().value();
    const auto it = std::find_if(all.begin(), all.end(), [&](const Category& c) { return c.id == *id; });
    ASSERT_NE(it, all.end());
    EXPECT_EQ(it->name, "Talks");
    EXPECT_EQ(it->kinds.size(), 2u);
    ASSERT_TRUE(library->remove_category(*id));
    EXPECT_EQ(library->remove_category(*id).error().code, ErrorCode::NotFound);
}

TEST_F(LibraryTest, CategoryNamesAreUniqueIgnoringCaseAndNotEmpty) {
    EXPECT_EQ(library->add_category("music", {}).error().code, ErrorCode::AlreadyExists);
    EXPECT_EQ(library->add_category("   ", {}).error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(library->update_category(Category{category("Books"), "MOVIES", {}}).error().code, ErrorCode::AlreadyExists);
    EXPECT_EQ(library->update_category(Category{999, "X", {}}).error().code, ErrorCode::NotFound);
}

TEST_F(LibraryTest, CategoryWithFoldersCannotBeRemoved) {
    fs::create_directories(dir.path() / "music");
    ASSERT_TRUE(library->add_root(category("Music"), dir.path() / "music"));
    EXPECT_EQ(library->remove_category(category("Music")).error().code, ErrorCode::InUse);
}

// ---------------------------------------------------------------- Roots

TEST_F(LibraryTest, AddRootWritesMarkerAndRecordsTheFolder) {
    fs::create_directories(dir.path() / "drive" / "Media" / "Music");
    mounts = {dir.path() / "drive"};
    auto root = library->add_root(category("Music"), dir.path() / "drive" / "Media" / ".." / "Media" / "Music");
    ASSERT_TRUE(root.has_value()) << root.error().message;
    EXPECT_EQ(root->name, "Music") << "defaults to the folder name";
    EXPECT_EQ(fs::path(root->path), fs::weakly_canonical(dir.path() / "drive" / "Media" / "Music")) << "normalized";
    EXPECT_EQ(root->path_in_volume, "Media/Music");
    EXPECT_TRUE(root->use_marker);
    EXPECT_TRUE(root->case_sensitive) << "ext4";
    EXPECT_EQ(root->status, RootStatus::Online);
    EXPECT_EQ(hoardor::file::detail::read_marker(root->path), root->uuid);
}

TEST_F(LibraryTest, AddRootAsWholeVolumeHasEmptyPathInVolume) {
    fs::create_directories(dir.path() / "drive");
    mounts = {dir.path() / "drive", dir.path()};
    auto root = library->add_root(category("Music"), dir.path() / "drive", "My HDD");
    ASSERT_TRUE(root.has_value());
    EXPECT_EQ(root->path_in_volume, "") << "the longest matching mount point wins";
    EXPECT_EQ(root->name, "My HDD");
}

TEST_F(LibraryTest, AddRootRejectsBadInput) {
    EXPECT_EQ(library->add_root(category("Music"), dir.path() / "missing").error().code, ErrorCode::NotFound);
    dir.write("file.mp3");
    EXPECT_EQ(library->add_root(category("Music"), dir.path() / "file.mp3").error().code, ErrorCode::InvalidArgument);
    fs::create_directories(dir.path() / "ok");
    EXPECT_EQ(library->add_root(999, dir.path() / "ok").error().code, ErrorCode::NotFound);
}

TEST_F(LibraryTest, RootsOnDifferentDrivesAndCategoriesAreIndependent) {
    fs::create_directories(dir.path() / "hdd_a" / "Music");
    fs::create_directories(dir.path() / "hdd_b" / "Films");
    fs::create_directories(dir.path() / "hdd_b" / "Series");
    ASSERT_TRUE(library->add_root(category("Music"), dir.path() / "hdd_a" / "Music"));
    ASSERT_TRUE(library->add_root(category("Movies"), dir.path() / "hdd_b" / "Films"));
    ASSERT_TRUE(library->add_root(category("Shows"), dir.path() / "hdd_b" / "Series"));
    EXPECT_EQ(library->roots().value().size(), 3u);
    EXPECT_EQ(library->roots(category("Movies")).value().size(), 1u);
}

TEST_F(LibraryTest, OverlappingRootsAreRejected) {
    fs::create_directories(dir.path() / "Music" / "Rock");
    fs::create_directories(dir.path() / "Musical");
    ASSERT_TRUE(library->add_root(category("Music"), dir.path() / "Music"));
    EXPECT_EQ(library->add_root(category("Music"), dir.path() / "Music").error().code, ErrorCode::Overlap);
    EXPECT_EQ(library->add_root(category("Music"), dir.path() / "Music" / "Rock").error().code, ErrorCode::Overlap);
    EXPECT_EQ(library->add_root(category("Music"), dir.path()).error().code, ErrorCode::Overlap);
    EXPECT_TRUE(library->add_root(category("Music"), dir.path() / "Musical")) << "a sibling with a common prefix is fine";
}

TEST_F(LibraryTest, FolderAlreadyInTheLibraryUnderAnotherPathIsRejected) {
    fs::create_directories(dir.path() / "a");
    fs::create_directories(dir.path() / "copy");
    auto first = library->add_root(category("Music"), dir.path() / "a");
    ASSERT_TRUE(first.has_value());
    // The same drive seen at a second location carries the same marker.
    ASSERT_TRUE(hoardor::file::detail::write_marker(dir.path() / "copy", first->uuid));
    EXPECT_EQ(library->add_root(category("Music"), dir.path() / "copy").error().code, ErrorCode::AlreadyExists);
}

TEST_F(LibraryTest, ExistingMarkerIsAdoptedWhenReAdding) {
    fs::create_directories(dir.path() / "a");
    const std::string uuid = hoardor::file::detail::new_uuid();
    ASSERT_TRUE(hoardor::file::detail::write_marker(dir.path() / "a", uuid));
    auto root = library->add_root(category("Music"), dir.path() / "a");
    ASSERT_TRUE(root.has_value());
    EXPECT_EQ(root->uuid, uuid);
}

TEST_F(LibraryTest, RootWithoutMarkerWritesNothing) {
    fs::create_directories(dir.path() / "a");
    auto root = library->add_root(category("Music"), dir.path() / "a", "", false);
    ASSERT_TRUE(root.has_value());
    EXPECT_FALSE(root->use_marker);
    EXPECT_FALSE(fs::exists(dir.path() / "a" / ".hoardor-root"));
}

TEST_F(LibraryTest, ReadOnlyFolderFallsBackToNoMarker) {
    fs::create_directories(dir.path() / "ro");
    fs::permissions(dir.path() / "ro", fs::perms::owner_read | fs::perms::owner_exec);
    struct Restore {
        fs::path p;
        ~Restore() { fs::permissions(p, fs::perms::owner_all); }
    } restore{dir.path() / "ro"};
    if (hoardor::file::detail::write_marker(dir.path() / "ro", hoardor::file::detail::new_uuid())) {
        fs::remove(dir.path() / "ro" / ".hoardor-root");
        GTEST_SKIP() << "running with privileges that ignore permissions (e.g. root)";
    }
    auto root = library->add_root(category("Music"), dir.path() / "ro");
    ASSERT_TRUE(root.has_value()) << "read-only storage is not an error";
    EXPECT_FALSE(root->use_marker);
}

TEST_F(LibraryTest, RootCategoryCanChangeButOnlyToAnExistingOne) {
    fs::create_directories(dir.path() / "a");
    auto root = library->add_root(category("Music"), dir.path() / "a");
    ASSERT_TRUE(library->set_root_category(root->id, category("Books")));
    EXPECT_EQ(library->root(root->id)->category_id, category("Books"));
    EXPECT_EQ(library->set_root_category(root->id, 999).error().code, ErrorCode::NotFound);
    EXPECT_EQ(library->set_root_category(999, category("Books")).error().code, ErrorCode::NotFound);
}

TEST_F(LibraryTest, RemoveRootKeepsItsMarkerOnDisk) {
    fs::create_directories(dir.path() / "a");
    auto root = library->add_root(category("Music"), dir.path() / "a");
    ASSERT_TRUE(library->remove_root(root->id));
    EXPECT_TRUE(library->roots().value().empty());
    EXPECT_TRUE(fs::exists(dir.path() / "a" / ".hoardor-root")) << "hoardor doesn't delete files on media drives";
    EXPECT_EQ(library->remove_root(root->id).error().code, ErrorCode::NotFound);
}

TEST(RootMarker, RejectsMalformedFiles) {
    TempDir dir;
    EXPECT_EQ(hoardor::file::detail::read_marker(dir.path()), std::nullopt);
    dir.write(".hoardor-root", 10);
    EXPECT_EQ(hoardor::file::detail::read_marker(dir.path()), std::nullopt);
    const std::string uuid = hoardor::file::detail::new_uuid();
    EXPECT_EQ(uuid.size(), 36u);
    EXPECT_EQ(uuid[14], '4') << "version 4";
    EXPECT_NE(uuid, hoardor::file::detail::new_uuid());
}

TEST_F(LibraryTest, InvalidSettingsAreRefusedAndNothingIsStored) {
    Settings s = Settings::defaults();
    s.mass_removal_threshold_percent = 150;
    s.sync_on_startup = true;
    const auto saved = library->save_settings(s);
    ASSERT_FALSE(saved.has_value());
    EXPECT_EQ(saved.error().code, ErrorCode::InvalidArgument);
    EXPECT_NE(saved.error().message.find("mass-removal threshold"), std::string::npos) << saved.error().message;
    EXPECT_FALSE(library->load_settings()->sync_on_startup) << "all or nothing";
}
