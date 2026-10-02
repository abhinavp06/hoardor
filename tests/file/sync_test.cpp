#include <hoardor/file/library.hpp>

#include "file/file_time.hpp"
#include "file/root_marker.hpp"
#include "support/library_fixture.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <set>

namespace fs = std::filesystem;
using namespace hoardor::file;
using hoardor::test::LibraryTest;

namespace {

constexpr std::int64_t old_mtime = 1'600'000'000'000'000'000;  // 2020, long settled

std::int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

class SyncTest : public LibraryTest {
protected:
    // Writes a settled media file under the music folder.
    fs::path put(const fs::path& relative, std::size_t size = 1, std::int64_t mtime = old_mtime) {
        const auto p = dir.write(fs::path("drive") / "Music" / relative, size);
        fs::last_write_time(p, detail::from_unix_ns(mtime));
        return p;
    }

    Root add_music(bool use_marker = true) {
        fs::create_directories(music());
        auto root = library->add_root(category("Music"), music(), "Music", use_marker);
        EXPECT_TRUE(root.has_value()) << root.error().message;
        return *root;
    }

    fs::path music() const { return dir.path() / "drive" / "Music"; }

    std::set<std::string> paths(RootId root) {
        std::set<std::string> out;
        for (const auto& e : all_entries(root)) out.insert(e.relative_path);
        return out;
    }
};

}

TEST_F(SyncTest, FirstSyncAddsMediaOfTheCategoryKindsOnly) {
    put("a.mp3");
    put("Album/b.flac");
    put("Album/cover.jpg");
    put("notes.txt");  // text isn't a Music kind
    put("film.mkv");   // neither is video
    const Root root = add_music();
    const RootSyncReport r = library->sync_root(root.id);
    EXPECT_EQ(r.outcome, RootSyncOutcome::Synced) << r.message;
    EXPECT_EQ(r.generation, 1);
    EXPECT_EQ(r.added, 3u);
    EXPECT_EQ(paths(root.id), (std::set<std::string>{"a.mp3", "Album/b.flac", "Album/cover.jpg"}));
    const Root after = library->root(root.id).value();
    EXPECT_EQ(after.generation, 1);
    EXPECT_EQ(after.file_count, 3u);
    EXPECT_EQ(after.status, RootStatus::Online);
    EXPECT_GT(after.last_sync_ns, 0);
}

TEST_F(SyncTest, AddedTimeIsSetOnceAndSurvivesChanges) {
    put("a.mp3");
    const Root root = add_music();
    const std::int64_t before = now_ns();
    ASSERT_EQ(library->sync_root(root.id).outcome, RootSyncOutcome::Synced);
    const Entry first = all_entries(root.id).at(0);
    EXPECT_GE(first.added_ns, before - 1'000'000'000);
    EXPECT_LE(first.added_ns, now_ns());

    put("a.mp3", 5);  // modified
    ASSERT_EQ(library->sync_root(root.id).modified, 1u);
    const Entry second = all_entries(root.id).at(0);
    EXPECT_EQ(second.id, first.id);
    EXPECT_EQ(second.added_ns, first.added_ns);
}

TEST_F(SyncTest, SecondSyncChangesNothing) {
    put("a.mp3");
    put("b.mp3");
    const Root root = add_music();
    library->sync_root(root.id);
    const RootSyncReport r = library->sync_root(root.id);
    EXPECT_EQ(r.outcome, RootSyncOutcome::Synced);
    EXPECT_EQ(r.added + r.modified + r.removed, 0u);
    EXPECT_EQ(r.unchanged, 2u);
    EXPECT_TRUE(library->changed_entries(root.id, r.generation).value().empty());
}

TEST_F(SyncTest, SizeOrMtimeChangesAreModificationsAndKeepTheId) {
    put("a.mp3", 10);
    put("b.mp3", 10);
    put("c.mp3", 10);
    const Root root = add_music();
    library->sync_root(root.id);
    const auto before = all_entries(root.id);
    put("a.mp3", 20);                     // re-tagged: bigger
    put("b.mp3", 10, old_mtime + 1'000);  // touched
    const RootSyncReport r = library->sync_root(root.id);
    EXPECT_EQ(r.modified, 2u);
    EXPECT_EQ(r.unchanged, 1u);
    const auto changed = library->changed_entries(root.id, r.generation).value();
    ASSERT_EQ(changed.size(), 2u);
    for (const auto& e : changed) {
        EXPECT_TRUE(std::any_of(before.begin(), before.end(), [&](const Entry& b) { return b.id == e.id; }));
    }
}

TEST_F(SyncTest, DeletedFilesAreRemovedBelowTheGuard) {
    for (int i = 0; i < 10; ++i) put("t" + std::to_string(i) + ".mp3");
    const Root root = add_music();
    library->sync_root(root.id);
    fs::remove(music() / "t3.mp3");
    const RootSyncReport r = library->sync_root(root.id);
    EXPECT_EQ(r.removed, 1u);
    EXPECT_FALSE(r.removals_held);
    EXPECT_EQ(all_entries(root.id).size(), 9u);
}

TEST_F(SyncTest, MassRemovalIsHeldUntilConfirmed) {
    for (int i = 0; i < 10; ++i) put("t" + std::to_string(i) + ".mp3");
    const Root root = add_music();
    library->sync_root(root.id);
    for (int i = 0; i < 5; ++i) fs::remove(music() / ("t" + std::to_string(i) + ".mp3"));
    const RootSyncReport r = library->sync_root(root.id);
    EXPECT_EQ(r.outcome, RootSyncOutcome::Synced);
    EXPECT_TRUE(r.removals_held);
    EXPECT_EQ(r.held_removals, 5u);
    EXPECT_EQ(r.removed, 0u);
    EXPECT_EQ(all_entries(root.id).size(), 10u);
    EXPECT_EQ(library->root(root.id)->held_removals, 5u);

    EXPECT_EQ(library->apply_held_removals(root.id).value(), 5u);
    EXPECT_EQ(all_entries(root.id).size(), 5u);
    EXPECT_EQ(library->root(root.id)->held_removals, 0u);
    EXPECT_EQ(library->root(root.id)->file_count, 5u);
}

TEST_F(SyncTest, MassRemovalThresholdIsASetting) {
    set_settings([](Settings& s) { s.mass_removal_threshold_percent = 60; });
    for (int i = 0; i < 10; ++i) put("t" + std::to_string(i) + ".mp3");
    const Root root = add_music();
    library->sync_root(root.id);
    for (int i = 0; i < 5; ++i) fs::remove(music() / ("t" + std::to_string(i) + ".mp3"));
    EXPECT_EQ(library->sync_root(root.id).removed, 5u);
}

TEST_F(SyncTest, MissingRootIsOfflineAndKeepsEverything) {
    put("a.mp3");
    const Root root = add_music();
    library->sync_root(root.id);
    fs::rename(music(), dir.path() / "elsewhere");  // the drive is unplugged
    const RootSyncReport r = library->sync_root(root.id);
    EXPECT_EQ(r.outcome, RootSyncOutcome::Offline);
    EXPECT_FALSE(r.message.empty());
    EXPECT_EQ(all_entries(root.id).size(), 1u);
    EXPECT_EQ(library->root(root.id)->status, RootStatus::Offline);

    fs::rename(dir.path() / "elsewhere", music());  // plugged back in
    EXPECT_EQ(library->sync_root(root.id).outcome, RootSyncOutcome::Synced);
    EXPECT_EQ(library->root(root.id)->status, RootStatus::Online);
}

TEST_F(SyncTest, DifferentDriveAtTheOldPathIsNotScanned) {
    put("a.mp3");
    const Root root = add_music();
    library->sync_root(root.id);
    fs::rename(music(), dir.path() / "unplugged");
    fs::create_directories(music());
    dir.write("drive/Music/other.mp3");
    ASSERT_TRUE(detail::write_marker(music(), detail::new_uuid()));  // another library folder took the letter
    const RootSyncReport r = library->sync_root(root.id);
    EXPECT_EQ(r.outcome, RootSyncOutcome::Offline);
    EXPECT_NE(r.message.find("different"), std::string::npos) << r.message;
    EXPECT_EQ(paths(root.id), (std::set<std::string>{"a.mp3"})) << "never 'everything deleted, everything new'";
}

TEST_F(SyncTest, DriveUnderANewLetterIsFoundThroughMountPoints) {
    put("a.mp3");
    put("b/c.mp3");
    mounts = {dir.path() / "drive", dir.path() / "drive2"};
    const Root root = add_music();
    EXPECT_EQ(root.path_in_volume, "Music");
    library->sync_root(root.id);
    const auto before = all_entries(root.id);

    fs::rename(dir.path() / "drive", dir.path() / "drive2");  // E: came back as F:
    const RootSyncReport r = library->sync_root(root.id);
    EXPECT_EQ(r.outcome, RootSyncOutcome::Synced) << r.message;
    EXPECT_TRUE(r.relocated);
    EXPECT_EQ(r.unchanged, 2u);
    EXPECT_EQ(fs::path(library->root(root.id)->path), (dir.path() / "drive2" / "Music").lexically_normal());
    const auto after = all_entries(root.id);
    ASSERT_EQ(after.size(), before.size());
    for (std::size_t i = 0; i < after.size(); ++i) EXPECT_EQ(after[i].id, before[i].id) << "ids survive a relocation";
}

TEST_F(SyncTest, EmptiedFolderWithoutMarkerIsTreatedAsUnmounted) {
    put("a.mp3");
    put("b.mp3");
    const Root root = add_music(false);
    library->sync_root(root.id);
    fs::remove(music() / "a.mp3");
    fs::remove(music() / "b.mp3");  // what an empty mount point looks like
    const RootSyncReport r = library->sync_root(root.id);
    EXPECT_EQ(r.outcome, RootSyncOutcome::Offline);
    EXPECT_EQ(all_entries(root.id).size(), 2u);
}

TEST_F(SyncTest, EmptyMountPointWithoutOurMarkerIsOffline) {
    put("a.mp3");
    const Root root = add_music();
    library->sync_root(root.id);
    fs::remove_all(music());
    fs::create_directories(music());  // the mount point folder stays behind, empty
    EXPECT_EQ(library->sync_root(root.id).outcome, RootSyncOutcome::Offline);
    EXPECT_EQ(all_entries(root.id).size(), 1u);
}

TEST_F(SyncTest, DriveLostMidSyncRemovesNothing) {
    for (int d = 0; d < 4; ++d) {
        for (int f = 0; f < 40; ++f) put("d" + std::to_string(d) + "/t" + std::to_string(f) + ".mp3");
    }
    set_settings([](Settings& s) { s.progress_interval_files = 1; });
    const Root root = add_music();
    ASSERT_EQ(library->sync_root(root.id).added, 160u);

    int calls = 0;
    const RootSyncReport r = library->sync_root(root.id, {}, [&](const SyncProgress&) {
        if (++calls == 3) fs::remove_all(music());  // unplugged while syncing
    });
    EXPECT_EQ(r.outcome, RootSyncOutcome::Offline) << r.message;
    EXPECT_EQ(r.removed, 0u);
    EXPECT_EQ(all_entries(root.id).size(), 160u);
    EXPECT_EQ(library->root(root.id)->generation, 1) << "an incomplete sync doesn't advance the generation";
    const auto errors = library->scan_errors(root.id).value();
    ASSERT_FALSE(errors.empty());
    EXPECT_TRUE(errors.back().relative_path.empty()) << "the root error is recorded";
}

TEST_F(SyncTest, CancelledSyncRemovesNothing) {
    for (int i = 0; i < 50; ++i) put("t" + std::to_string(i) + ".mp3");
    set_settings([](Settings& s) { s.progress_interval_files = 5; });
    const Root root = add_music();
    library->sync_root(root.id);
    fs::remove(music() / "t1.mp3");

    std::stop_source stop;
    const RootSyncReport r = library->sync_root(root.id, stop.get_token(), [&](const SyncProgress& p) {
        if (p.scan.files_emitted >= 10) stop.request_stop();
    });
    EXPECT_EQ(r.outcome, RootSyncOutcome::Cancelled);
    EXPECT_EQ(all_entries(root.id).size(), 50u);
    EXPECT_EQ(library->root(root.id)->generation, 1);
}

TEST_F(SyncTest, StopRequestedBeforeSyncStarts) {
    const Root root = add_music();
    std::stop_source stop;
    stop.request_stop();
    const SyncReport r = library->sync(std::nullopt, stop.get_token());
    EXPECT_TRUE(r.cancelled);
    EXPECT_EQ(library->root(root.id)->generation, 0);
}

TEST_F(SyncTest, FileBeingCopiedIsUnsettledUntilItSettles) {
    const auto file = put("copying.mp3", 1, now_ns());
    const Root root = add_music();
    RootSyncReport r = library->sync_root(root.id);
    EXPECT_EQ(r.added, 1u);
    EXPECT_EQ(r.unsettled, 1u);
    EXPECT_TRUE(all_entries(root.id).at(0).unsettled);

    fs::last_write_time(file, detail::from_unix_ns(old_mtime));  // the copy finished long ago
    r = library->sync_root(root.id);
    EXPECT_EQ(r.modified, 1u) << "settling makes it worth reading now";
    EXPECT_EQ(r.unsettled, 0u);
    EXPECT_FALSE(all_entries(root.id).at(0).unsettled);
}

TEST_F(SyncTest, FutureMtimeIsNotUnsettledForever) {
    put("camera.jpg", 1, now_ns() + 86'400'000'000'000);  // a day ahead
    const Root root = add_music();
    EXPECT_EQ(library->sync_root(root.id).unsettled, 0u);
}

TEST_F(SyncTest, ChangingTheCategoryAppliesItsKinds) {
    put("song.mp3");
    put("notes.txt");
    set_settings([](Settings& s) { s.mass_removal_threshold_percent = 100; });
    const Root root = add_music();
    library->sync_root(root.id);
    EXPECT_EQ(paths(root.id), (std::set<std::string>{"song.mp3"}));
    ASSERT_TRUE(library->set_root_category(root.id, category("Books")));
    library->sync_root(root.id);
    EXPECT_EQ(paths(root.id), (std::set<std::string>{"notes.txt"}));
}

TEST_F(SyncTest, CaseOnlyRenameOnCaseInsensitiveRootKeepsTheId) {
    put("Song.mp3");
    const Root root = add_music();
    ASSERT_TRUE(library->set_root_case_sensitive(root.id, false));
    library->sync_root(root.id);
    const EntryId id = all_entries(root.id).at(0).id;
    fs::rename(music() / "Song.mp3", music() / "song.mp3");
    const RootSyncReport r = library->sync_root(root.id);
    EXPECT_EQ(r.modified, 1u);
    EXPECT_EQ(r.added, 0u);
    const auto entries = all_entries(root.id);
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].id, id);
    EXPECT_EQ(entries[0].relative_path, "song.mp3");
}

TEST_F(SyncTest, CaseInsensitiveIsRefusedWhenNamesCollide) {
    put("a.mp3");
    put("A.mp3");
    const Root root = add_music();
    library->sync_root(root.id);
    if (all_entries(root.id).size() != 2) GTEST_SKIP() << "case-insensitive file system";
    EXPECT_EQ(library->set_root_case_sensitive(root.id, false).error().code, ErrorCode::InvalidArgument);
    EXPECT_TRUE(library->root(root.id)->case_sensitive) << "rolled back";
}

TEST_F(SyncTest, RemovingARootRemovesItsEntries) {
    put("a.mp3");
    const Root root = add_music();
    library->sync_root(root.id);
    ASSERT_TRUE(library->remove_root(root.id));
    auto st = db->prepare("SELECT COUNT(*) FROM file_entries");
    ASSERT_TRUE(st->step().value());
    EXPECT_EQ(st->column_int64(0), 0);
}

TEST_F(SyncTest, SyncOfOneCategoryOnlyTouchesItsRoots) {
    put("a.mp3");
    const Root music_root = add_music();
    dir.write("drive/Films/f.mkv");
    const Root films = library->add_root(category("Movies"), dir.path() / "drive" / "Films").value();

    const SyncReport section = library->sync(category("Music"));
    ASSERT_EQ(section.roots.size(), 1u);
    EXPECT_EQ(section.roots[0].root_id, music_root.id);
    EXPECT_EQ(library->root(films.id)->generation, 0);

    std::vector<std::pair<std::size_t, std::size_t>> positions;
    const SyncReport global = library->sync(std::nullopt, {}, [&](const SyncProgress& p) {
        positions.emplace_back(p.root_index, p.root_count);
    });
    EXPECT_EQ(global.roots.size(), 2u);
    EXPECT_FALSE(global.cancelled);
    EXPECT_EQ(library->root(films.id)->generation, 1);
    EXPECT_EQ(positions.front(), (std::pair<std::size_t, std::size_t>{0, 2}));
    EXPECT_EQ(positions.back(), (std::pair<std::size_t, std::size_t>{1, 2}));
}

TEST_F(SyncTest, OneOfflineRootDoesNotStopTheOthers) {
    put("a.mp3");
    const Root first = add_music();
    dir.write("other/b.mp3");
    const Root second = library->add_root(category("Music"), dir.path() / "other").value();
    fs::rename(music(), dir.path() / "gone");
    const SyncReport r = library->sync(category("Music"));
    ASSERT_EQ(r.roots.size(), 2u);
    EXPECT_EQ(r.roots[0].outcome, RootSyncOutcome::Offline);
    EXPECT_EQ(r.roots[1].outcome, RootSyncOutcome::Synced);
    EXPECT_EQ(r.roots[1].root_id, second.id);
    (void)first;
}

TEST_F(SyncTest, SmallBatchesGiveTheSameResult) {
    for (int i = 0; i < 25; ++i) put("t" + std::to_string(i) + ".mp3");
    set_settings([](Settings& s) {
        s.batch_max_rows = 1;
        s.progress_interval_files = 3;
    });
    const Root root = add_music();
    int calls = 0;
    const RootSyncReport r = library->sync_root(root.id, {}, [&](const SyncProgress&) { ++calls; });
    EXPECT_EQ(r.added, 25u);
    EXPECT_GE(calls, 8) << "start, every 3 files, end";
}

TEST_F(SyncTest, PagingVisitsEveryEntryOnce) {
    for (int i = 0; i < 25; ++i) put("t" + std::to_string(i) + ".mp3");
    const Root root = add_music();
    const auto gen = library->sync_root(root.id).generation;
    std::set<EntryId> seen;
    EntryId after = 0;
    int pages = 0;
    while (true) {
        auto page = library->changed_entries(root.id, gen, after, 10).value();
        if (page.empty()) break;
        ++pages;
        for (const auto& e : page) EXPECT_TRUE(seen.insert(e.id).second);
        after = page.back().id;
    }
    EXPECT_EQ(seen.size(), 25u);
    EXPECT_EQ(pages, 3);
}

TEST_F(SyncTest, ResolveForPlayback) {
    put("Album/a.mp3");
    const Root root = add_music();
    library->sync_root(root.id);
    const EntryId id = all_entries(root.id).at(0).id;
    auto path = library->resolve(id);
    ASSERT_TRUE(path.has_value()) << path.error().message;
    EXPECT_TRUE(fs::exists(*path));

    fs::remove(music() / "Album" / "a.mp3");
    EXPECT_EQ(library->resolve(id).error().code, ErrorCode::FileMissing);
    fs::rename(music(), dir.path() / "gone");
    EXPECT_EQ(library->resolve(id).error().code, ErrorCode::RootOffline);
    EXPECT_EQ(library->resolve(999).error().code, ErrorCode::NotFound);
}

TEST_F(SyncTest, ManualRelocationWithMarker) {
    put("a.mp3");
    const Root root = add_music();
    library->sync_root(root.id);
    fs::rename(music(), dir.path() / "NewHome");
    auto moved = library->relocate_root(root.id, dir.path() / "NewHome");
    ASSERT_TRUE(moved.has_value()) << moved.error().message;
    EXPECT_EQ(library->sync_root(root.id).unchanged, 1u);
}

TEST_F(SyncTest, ManualRelocationRefusesAnotherRootsFolder) {
    const Root root = add_music();
    fs::create_directories(dir.path() / "other");
    ASSERT_TRUE(detail::write_marker(dir.path() / "other", detail::new_uuid()));
    EXPECT_EQ(library->relocate_root(root.id, dir.path() / "other").error().code, ErrorCode::InvalidArgument);
}

TEST_F(SyncTest, ManualRelocationWithoutMarkerChecksASample) {
    for (int i = 0; i < 10; ++i) put("t" + std::to_string(i) + ".mp3", 100 + i);
    const Root root = add_music(false);
    library->sync_root(root.id);

    fs::create_directories(dir.path() / "wrong");
    dir.write("wrong/t0.mp3", 100);  // only one matching file
    EXPECT_EQ(library->relocate_root(root.id, dir.path() / "wrong").error().code, ErrorCode::InvalidArgument);

    fs::rename(music(), dir.path() / "copy");
    auto moved = library->relocate_root(root.id, dir.path() / "copy");
    ASSERT_TRUE(moved.has_value()) << moved.error().message;
    EXPECT_FALSE(moved->use_marker);
    EXPECT_EQ(library->sync_root(root.id).unchanged, 10u);
}

TEST_F(SyncTest, UnreadableSubfolderKeepsItsEntries) {
    put("open/a.mp3");
    put("locked/b.mp3");
    const Root root = add_music();
    library->sync_root(root.id);
    const auto locked = music() / "locked";
    fs::permissions(locked, fs::perms::none);
    struct Restore {
        fs::path p;
        ~Restore() { fs::permissions(p, fs::perms::owner_all); }
    } restore{locked};
    std::error_code ec;
    fs::directory_iterator probe(locked, ec);
    if (!ec) GTEST_SKIP() << "running with privileges that ignore permissions (e.g. root)";

    const RootSyncReport r = library->sync_root(root.id);
    EXPECT_EQ(r.outcome, RootSyncOutcome::Synced);
    EXPECT_EQ(r.removed, 0u);
    EXPECT_EQ(r.errors, 1u);
    EXPECT_EQ(paths(root.id), (std::set<std::string>{"open/a.mp3", "locked/b.mp3"}));
    const auto errors = library->scan_errors(root.id).value();
    ASSERT_EQ(errors.size(), 1u);
    EXPECT_EQ(errors[0].relative_path, "locked");
    EXPECT_TRUE(errors[0].is_directory);
}
