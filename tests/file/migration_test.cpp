// Upgrading a v0.1.0 library (file migration 1) to the current schema keeps every entry
// and its id, and fills in the new columns (features/media_listing.md §6).

#include <hoardor/db/database.hpp>
#include <hoardor/file/library.hpp>

#include <gtest/gtest.h>

using namespace hoardor;
using namespace hoardor::file;

namespace {

// file migration 1 exactly as shipped in v0.1.0 (a shipped migration never changes).
constexpr const char* v1_schema = R"sql(
CREATE TABLE db_migrations (component TEXT PRIMARY KEY, version INTEGER NOT NULL);
INSERT INTO db_migrations VALUES ('file', 1);
CREATE TABLE file_settings (key TEXT PRIMARY KEY, value TEXT NOT NULL);
CREATE TABLE file_categories (id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE COLLATE NOCASE, kinds TEXT NOT NULL);
CREATE TABLE file_roots (
    id INTEGER PRIMARY KEY, uuid TEXT NOT NULL UNIQUE,
    category_id INTEGER NOT NULL REFERENCES file_categories(id),
    name TEXT NOT NULL, path TEXT NOT NULL, path_in_volume TEXT NOT NULL,
    use_marker INTEGER NOT NULL, case_sensitive INTEGER NOT NULL,
    status INTEGER NOT NULL DEFAULT 0, generation INTEGER NOT NULL DEFAULT 0,
    last_sync_ns INTEGER NOT NULL DEFAULT 0, file_count INTEGER NOT NULL DEFAULT 0,
    held_removals INTEGER NOT NULL DEFAULT 0);
CREATE TABLE file_entries (
    id INTEGER PRIMARY KEY, root_id INTEGER NOT NULL REFERENCES file_roots(id) ON DELETE CASCADE,
    relative_path TEXT NOT NULL, path_key TEXT NOT NULL, size INTEGER NOT NULL, mtime_ns INTEGER NOT NULL,
    kind INTEGER NOT NULL, unsettled INTEGER NOT NULL DEFAULT 0, seen_generation INTEGER NOT NULL,
    changed_generation INTEGER NOT NULL, UNIQUE (root_id, path_key));
CREATE INDEX file_entries_changed ON file_entries (root_id, changed_generation, id);
CREATE TABLE file_scan_errors (
    root_id INTEGER NOT NULL REFERENCES file_roots(id) ON DELETE CASCADE, relative_path TEXT NOT NULL,
    is_directory INTEGER NOT NULL, message TEXT NOT NULL, generation INTEGER NOT NULL);
CREATE INDEX file_scan_errors_root ON file_scan_errors (root_id, generation);
INSERT INTO file_categories (name, kinds) VALUES
    ('Music', 'audio,image'), ('Movies', 'video,subtitle,image'), ('Shows', 'video,subtitle,image'), ('Books', 'text,image');
)sql";

db::Database v1_library() {
    auto database = db::Database::open_in_memory();
    EXPECT_TRUE(database.has_value());
    EXPECT_TRUE(database->exec(v1_schema));
    EXPECT_TRUE(database->exec(
        "INSERT INTO file_roots (id, uuid, category_id, name, path, path_in_volume, use_marker, case_sensitive) "
        "VALUES (1, 'u-1', 1, 'Music', '/m', 'm', 0, 1);"
        "INSERT INTO file_entries (id, root_id, relative_path, path_key, size, mtime_ns, kind, seen_generation, "
        "changed_generation) VALUES (7, 1, 'a.flac', 'a.flac', 10, 1500, 1, 1, 1), (9, 1, 'b.flac', 'b.flac', 20, 2500, 1, 1, 1);"));
    return std::move(*database);
}

}

TEST(FileMigration, AV010LibraryKeepsItsEntriesAndGetsAddedTimes) {
    db::Database database = v1_library();
    auto library = Library::open(database);
    ASSERT_TRUE(library.has_value()) << library.error().message;

    const auto entries = library->entries(1).value();
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[0].id, 7);
    EXPECT_EQ(entries[0].added_ns, 1500);  // its mtime: the closest guess for an old entry
    EXPECT_EQ(entries[1].id, 9);
    EXPECT_EQ(entries[1].added_ns, 2500);
}

TEST(FileMigration, VideoCategoriesGainInfoOnce) {
    db::Database database = v1_library();
    ASSERT_TRUE(database.exec("UPDATE file_categories SET kinds = 'video,info' WHERE name = 'Shows'"));
    auto library = Library::open(database);
    ASSERT_TRUE(library.has_value());
    const auto categories = library->categories().value();
    EXPECT_EQ(categories[1].kinds,
              (std::vector<FileKind>{FileKind::Video, FileKind::Subtitle, FileKind::Image, FileKind::Info}));
    EXPECT_EQ(categories[2].kinds, (std::vector<FileKind>{FileKind::Video, FileKind::Info}));  // already had it
    EXPECT_EQ(categories[0].kinds, (std::vector<FileKind>{FileKind::Audio, FileKind::Image}));  // untouched
}

TEST(FileMigration, SavedExtensionMapGainsNfo) {
    db::Database database = v1_library();
    ASSERT_TRUE(database.exec("INSERT INTO file_settings VALUES ('extension_kinds', 'flac=audio' || char(10) || 'mkv=video')"));
    auto library = Library::open(database);
    ASSERT_TRUE(library.has_value());
    const Settings s = library->load_settings().value();
    EXPECT_EQ(s.extension_kinds.size(), 3u);
    EXPECT_EQ(s.extension_kinds.at("nfo"), FileKind::Info);
}

TEST(FileMigration, AUserMappingOfNfoIsKept) {
    db::Database database = v1_library();
    ASSERT_TRUE(database.exec("INSERT INTO file_settings VALUES ('extension_kinds', 'nfo=text')"));
    auto library = Library::open(database);
    ASSERT_TRUE(library.has_value());
    EXPECT_EQ(library->load_settings().value().extension_kinds.at("nfo"), FileKind::Text);
}

TEST(FileMigration, TheDefaultBooksCategoryGoesUnlessItIsInUseOrChanged) {
    {
        db::Database database = v1_library();
        auto library = Library::open(database);
        ASSERT_TRUE(library.has_value());
        const auto categories = library->categories().value();
        ASSERT_EQ(categories.size(), 3u);
        EXPECT_EQ(categories[2].name, "Shows");
    }
    {  // a folder in Books: it's the user's now
        db::Database database = v1_library();
        ASSERT_TRUE(database.exec("INSERT INTO file_roots (id, uuid, category_id, name, path, path_in_volume, use_marker, "
                                  "case_sensitive) VALUES (2, 'u-2', 4, 'Books', '/b', 'b', 0, 1)"));
        auto library = Library::open(database);
        ASSERT_TRUE(library.has_value());
        EXPECT_EQ(library->categories().value().size(), 4u);
    }
    {  // kinds the user changed
        db::Database database = v1_library();
        ASSERT_TRUE(database.exec("UPDATE file_categories SET kinds = 'text' WHERE name = 'Books'"));
        auto library = Library::open(database);
        ASSERT_TRUE(library.has_value());
        EXPECT_EQ(library->categories().value().size(), 4u);
    }
}
