# Unreleased

Work that is done but not yet part of a version. The newest entries come first. When a version is cut, these entries move unchanged into `v<version>.md`, and this file is emptied (see `README.md`).

### No Books category by default (2026-10-03, branch `abhinavp06/MEDIA_LISTING`)

**Summary:** The user asked to remove Books from TYLI's sidebar and Settings. Text has no build yet (books and blogs are a separate, unplanned build), so an empty Books section only gets in the way.
- **File migration 3** deletes the seeded Books category when it's untouched: name `Books`, kinds `text,image`, and no folders. A Books category with folders, or with kinds the user changed, stays.
- **New libraries** still get Books from migration 1 (a shipped migration is never edited), and migration 3 removes it right after, so they start with Music, Movies, and Shows.
- `FileKind::Text` and its extensions are unchanged. A Books category can be added again by hand at any time.

**Decision:** delete the category rather than hide it in TYLI. A hidden category would still be data with no use, and TYLI would need a rule for which categories to hide.

**Tests:** 192 pass.
- `FileMigration.TheDefaultBooksCategoryGoesUnlessItIsInUseOrChanged`: the default goes; one with a folder stays; one with changed kinds stays.
- **Updated:** the tests that used the seeded Books (`LibraryTest`'s seeded categories, reopening, renaming clashes, moving a root, and `SyncTest.ChangingTheCategoryAppliesItsKinds`, which now adds its own Books).

**Files**
- `src/file/library.cpp`
- `tests/file/{library_test,sync_test,migration_test}.cpp`
- `DOCUMENTATION/application/{ARCHITECTURE.md, DATABASE.md, CODE_TREE.md, CODE_TREE.html, engines/file.md}`

### Videos find their own poster and .nfo in big folders (2026-10-03, branch `abhinavp06/MEDIA_LISTING`)

**Summary:** The user's Windows test: "movie and TV show covers aren't being rendered for the majority of the media. Music albums seem to work fine."
- **Cause:** `master` gave `video::read` the first 200 companions of a video's folder (images, subtitles, `.nfo`), in name order.
  - In a shared folder of hundreds of movies, the first ~50 movies' sidecars filled the list, so every later movie lost its poster and its `.nfo` (title from the file name).
  - The same happened to an episode whose season folder holds many subtitle files: the show's `poster.jpg` one folder up was never reached.
  - Music was fine because TYLI's album art uses its own lookup, with any image next to a track as a fallback.
- **Fix:** `video` names what it can use (`companion_prefixes`), and `file::Library::companions` returns only files starting with those names, one index lookup per prefix. A movie among a thousand others now costs a few index seeks and always finds its own files.
- **Also:**
  - **`<name>.jpg`** (Plex's poster next to a movie in a shared folder) counts as the movie's poster, after `<name>-poster`.
  - **Embedded posters:** besides `cover.*`, Matroska image attachments with "cover" or "poster" in their name count (`small_cover.jpg`, `cover_land.jpg`). Attachments that aren't images (fonts) never do.
- **Video migration 3** marks every video for one more read, so existing libraries pick up the missing posters and `.nfo` descriptions on the next tag-reading pass. The rows stay listed meanwhile.

**Decisions**
- **Name prefixes rather than a higher limit:** a limit only moves the cliff, and reading a whole folder of thousands of files for each of its videos grows with the square of the folder.
- **Common spellings on case-sensitive roots:** a prefix also matches in lowercase, Capitalized, and UPPERCASE (`Poster.jpg`, `FOLDER.JPG`), so Linux and macOS roots behave like Windows for the usual names, without a full scan.
- **A one-time re-read (migration) rather than a "companions changed" detector.** See the follow-ups.

**Tests:** 191 pass (8 new).
- `CompanionsTest.PrefixesFindAMoviesSidecarsInAFlatFolderOfHundreds`: 300 movies with 4 files each; without prefixes the 200 limit hides Movie 250's files, with prefixes they're found; "Movie 25." doesn't match "Movie 250.nfo".
- `CompanionsTest.PrefixesNearestFolderFirstAndEachFileOnce`: 250 subtitles in a season folder no longer hide the show's poster; nearest folder first; overlapping prefixes list a file once.
- `CompanionsTest.PrefixesMatchCommonSpellings`: `Poster.jpg`, `FOLDER.JPG`, a stem in another case, and a mixed-case name on a case-insensitive root.
- `VideoRead.PosterNamedLikeTheMovie`, `VideoRead.EmbeddedPosterUnderOtherNamesButNeverAFont`, `VideoCompanionPrefixes.TheFilesOwnNameThenTheCommonNames`.
- `VideoLibraryTest.MigrationThreeReadsEveryVideoAgain`.
- `MetadataPassTest.EachMovieFindsItsPosterInAFlatFolder`: the end-to-end reproduction, with 300 other sidecars sorting before Zodiac's. It fails on the old code (no poster, title from the name) and passes now.

**Files**
- `include/hoardor/file/library.hpp`, `src/file/library.cpp` (`companions(…, prefixes)`, `spellings`)
- `include/hoardor/video/video.hpp`, `src/video/read.cpp` (`companion_prefixes`, `pick_poster`, `cover_attachment`)
- `src/video/library.cpp` (migration 3)
- `src/master/sync_worker.cpp`
- `tests/file/companions_test.cpp`, `tests/video/read_test.cpp`, `tests/video/library_test.cpp`, `tests/master/metadata_pass_test.cpp`
- `DOCUMENTATION/application/{DATABASE.md, CODE_TREE.md, CODE_TREE.html, features/media_listing.md, engines/file.md, engines/video.md, engines/master.md}`

**Known limitations and follow-ups**
- **A poster added later isn't noticed:** a video is read again only when the video file changes. An image or `.nfo` added beside an unchanged video waits for the next migration or a manual re-read. Follow-up: when a sync sees an image or `.nfo` added or changed, mark the videos in that folder (and up to two levels below) for re-reading.
- **No local art at all** (Plex keeps its artwork in its own database) still means no poster. The option then is a frame from the video as its poster (a new hoardor feature).
- `season01-poster.jpg` (a season's poster in the show folder) and `fanart` aren't used yet.

### Full-text search (audio and video) (2026-10-02, branch `abhinavp06/MEDIA_LISTING`)

**Summary:** The user asked for a search bar. hoardor gets a generic `Field::Search` filter in both engines, in keeping with the generic-API rule:
- **Matching:** every typed word must match, as a prefix, ignoring case and accents.
- **Audio** searches title, album, album artist, artists, and genres.
- **Video** searches title, show, genres, and directors.
- **Combining:** it works with every other filter, order, and grouping (e.g. albums matching "radio").

**Added**
- **SQLite with `SQLITE_ENABLE_FTS5`** (`third_party/CMakeLists.txt`): the full-text module built into the same amalgamation, with no new dependency.
- **audio migration 2:** an `audio_search` FTS5 table (`unicode61 remove_diacritics 2`, rowid = entry_id), backfilled from `audio_tracks`, and three triggers keeping it current on insert, update, and delete (cascades from `file_entries` fire them too). Unreadable rows aren't indexed.
- **video migration 2:** `video_search`, the same way.
- **`media::search_query(text)`:** user words become `"word"*` terms. Quotes, operators, and punctuation in the input are only separators, so no FTS syntax leaks through. No words means nothing matches.
- **`FieldSql::search_table`:** the filter becomes `id IN (SELECT rowid FROM <table> WHERE <table> MATCH ?)`.

**Decision**
- **FTS5 over `LIKE '%…%'`:** an index instead of a full scan, word prefixes, and accent folding.
- **Triggers rather than code in `store()`:** they also cover deletes by cascade.

**Tests:** 183 pass.
- `AudioLibraryTest.SearchFindsWordPrefixesAcrossFieldsIgnoringAccents`:
  - matches in album artist and album, all words required, title prefixes, genres
  - diacritics ("bjork" finds Björk), case, punctuation (`ac/dc`), stray quotes, empty input
  - unreadable rows excluded, groups of matching albums
  - reindexing on retag, emptied on removal
- `VideoLibraryTest.SearchTitlesShowsAndDirectors`

**Files**
- `third_party/CMakeLists.txt`
- `src/media/query.{hpp,cpp}`
- `include/hoardor/{audio/audio,video/video}.hpp`, `src/{audio,video}/library.cpp`
- `tests/{audio,video}/library_test.cpp`
- docs

### Sync and metadata reading run one worker per physical drive (2026-10-02, branch `abhinavp06/MEDIA_LISTING`)

**Summary:** This is the per-drive parallelism the user asked for, and the start of file engine phase 4. Folders on different physical drives now sync and have their metadata read at the same time, one worker per drive, never two on one disk (ARCHITECTURE §6: two readers on a spinning disk thrash its head).

**Added**
- **`file::device_of(path)`** (`include/hoardor/file/mount_points.hpp`) and `file::DeviceLookup`:
  - **Linux** (`device_linux.cpp`): the parent block device from `/sys/dev/block/MAJ:MIN`, so two partitions of one disk match. Network shares and FUSE fall back to the device id.
  - **Windows** (`device_windows.cpp`): the disk number behind the volume (`IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS`), else the volume serial.
  - **Other systems** (`device_other.cpp`): `st_dev`. On macOS, partitions of one disk count as different drives until a DiskArbitration backend exists.
- **The `file::Settings::parallel_devices` setting** (default 4, range 1–16; 1 = one drive at a time): persisted and validated.
- **`SyncWorker`:**
  - `start(…, device_of)`, injectable for tests
  - `sync(scope, stop)`: roots grouped by drive, up to `parallel_devices` workers, each with its own connection; reports keep the roots' order, and progress carries the global root index
  - `read_metadata`: the same split, using `pending(…, root)`
- **`audio` / `video`:** `pending(category, after, limit, root)` and `pending_count(category, root)` take an optional root.
- **`tests/tsan.supp`:** SQLite's WAL index is lock-free by design (file locks and memory barriers that ThreadSanitizer can't see). Two writing connections reported races only inside `wal*` functions; those are suppressed, and nothing else is.

**Decisions**
- **One worker per physical drive, not per folder or per CPU core.**
  - Offline or unidentifiable roots share one group.
  - Within a drive, roots run one after another.
  - SQLite writers take turns: each drive commits short batches, under `busy_timeout`.
- **No SSD detection yet:** one worker per drive even on SSDs. It's simple and never thrashes. Several workers per SSD could come later if daily use shows a need.

**Tests:** 181 pass.
- `MetadataPassTest.TwoDrivesSyncAndReadInParallel`: two pretend drives sync on two threads, keep the report order, and read 300 tracks.
- `MetadataPassTest.OneDriveAtATimeWhenTheSettingSaysSo`
- Settings round trip and validation for `parallel_devices`.
- ThreadSanitizer is clean on the worker tests with `tests/tsan.supp`.

**Known limitations**
- `device_of` on Windows and macOS is compiled in CI but untested on real drives (the VM has one disk). The user's Windows run will tell.

**Files**
- `include/hoardor/file/{mount_points,settings}.hpp`
- `src/file/platform/device_{linux,windows,other}.cpp`
- `src/file/{settings,library}.cpp`
- `include/hoardor/{audio/audio,video/video}.hpp`, `src/{audio,video}/library.cpp`
- `include/hoardor/master/sync_worker.hpp`, `src/master/sync_worker.cpp`
- `CMakeLists.txt`, `CLAUDE.md` (the TSan command)
- tests and docs

### Metadata reading split from sync, and 8x faster with embedded covers (2026-10-02, branch `abhinavp06/MEDIA_LISTING`)

**Summary:** The user reported that syncing got slow once metadata reading was added. There were two causes:
- **Reading ran inside the sync.** The sync didn't finish, and a new sync waited, until every file was read.
- **Embedded covers were decoded.** ffmpeg's stream probe, which FLAC always needs because its header leaves the sample rate unknown, decoded each embedded cover picture to learn its pixel format. With a 3 MB PNG cover that meant 20 files per second instead of 1,700.

**Changed**
- **`master::SyncWorker` runs two kinds of job.**
  - A sync, as before, then **metadata reading as its own job**, queued after the sync (one per scope; a pass over everything covers a category's).
  - Syncs always go ahead of metadata jobs. A sync request **pauses** a running pass, which goes back in the queue and resumes after the sync with no `on_metadata_finished` in between.
  - New `syncing()` and `reading()`.
  - `MetadataProgress` and `MetadataReport` gain `elapsed_ms`.
- **`src/media/ffmpeg.cpp`:** before the probe, cover-picture streams get a placeholder pixel format, so ffmpeg doesn't decode them. `embedded_cover()` still returns the encoded bytes, untouched.

**Added**
- `playground/media/read_playground.cpp` (`hoardor_read <folder>`): metadata reading speed (files/s and, on Linux, bytes read) on a real drive.
- `MetadataPassTest.ASyncRequestPausesThePassWhichThenResumes`: it records `paused=yes` in the test report when the pause path ran.

**Measured** (this VM, warm cache, 100 FLACs of 20 s; `hoardor_read`):

| Set | Before | After |
|---|---|---|
| No cover | 1,395 files/s, 33 KB read per file | 1,733 files/s |
| 3 MB PNG cover | 20 files/s, 3.1 MB per file | **159 files/s**, 3.1 MB per file |

- **What's left is reading the picture itself:** ffmpeg's FLAC reader loads it in full. On an HDD that's about 20 ms per file on top of the seek.
- **Ways out:**
  - parallel reading across drives (file engine phase 4, next)
  - an own minimal FLAC/ID3 header reader, if it's still the bottleneck after measuring on the user's drives (rejected for now: that's tag parsing hoardor would own)

**Files**
- `include/hoardor/master/sync_worker.hpp`, `src/master/sync_worker.cpp`, `src/media/ffmpeg.cpp`
- `playground/CMakeLists.txt`, `playground/media/read_playground.cpp`
- `tests/master/metadata_pass_test.cpp`
- Docs: `engines/master.md`, `features/media_listing.md` §8b, `CODE_TREE.md`/`.html`

**Tests:** 179 pass.

### Media library v1, phases 1–4 (hoardor): ffmpeg metadata, audio and video engines, generic queries, metadata after sync (2026-10-02, branch `abhinavp06/MEDIA_LISTING`)

**Summary:** hoardor now reads what's inside audio and video files and answers generic queries over them, built from `features/media_listing.md` (draft v2, approved 2026-10-02):
- **ffmpeg** reads tags and stream information.
- **Two new engines:** `audio` and `video` store it.
- **Queries:** filter, order, group, and count by fields, with keyset cursors.
- **When:** the sync worker reads new and changed files right after each sync, while the drives are awake.
- **Movie and show info** comes from `.nfo` files (pugixml), embedded tags, or names as a last resort.

TYLI builds every layout from these queries.

**Phase 1** (committed earlier as `b2d8987`):
- ffmpeg and pugixml in the build
- `core::Cursor` / `core::Page` and the sort-key helpers
- `file`: `added_ns`, `FileKind::Info`, `companions()`, migration 2

**Added**
- **Internal media layer (`src/media/`):**
  - `ffmpeg.*`: open a file with a bounded probe, its streams, attached picture, duration, tags.
  - `query.*`: the generic query builder both engines describe their fields to.
- **`audio`** (`include/hoardor/audio/audio.hpp`, `src/audio/`, `engines/audio.md`):
  - `read` / `embedded_cover`
  - `Library` with `pending`, `store`, `store_error`, `remove_unused_names`, `tracks`, `groups`, `count`, `group_count`, `track`
  - audio migration 1: `audio_tracks`, `audio_names`, `audio_track_names`
- **`video`** (`include/hoardor/video/video.hpp`, `src/video/`, `engines/video.md`):
  - `read(path, companions)` / `embedded_poster`
  - `.nfo` and name parsing (`src/video/sources.*`)
  - the same `Library` shape
  - video migration 1: `video_items`, `video_names`, `video_item_names`
- **`master`:**
  - `SyncWorker` reads metadata after each non-cancelled sync (`read_metadata`), with `MetadataProgress` / `MetadataReport` callbacks
  - a file or root that vanished is skipped, not marked unreadable
- **Benchmark:** `benchmarks/audio/query_benchmark.cpp`.

**Decisions and alternatives rejected**
- **Generic APIs** (the user): no album or layout concepts in hoardor, and no copy grouping. A "Field" enum per engine, with SQL from fixed fragments, never from caller strings.
- **Index hints:** SQLite treats `GROUP BY` columns as a set and used the wrong album index, sorting every group. Value-ordered groupings now name their index, but only for broad filters. Alternatives rejected:
  - depending on index creation order (fragile)
  - SQLite's test-control optimization flags (not public API)
- **Cursors:** row-value seeks when every key sorts the same way, an OR chain otherwise. For value-ordered groups the cursor goes in `WHERE`, not `HAVING`.
- **Name filters:** one link row joined through a name-id subquery, instead of `EXISTS`, so SQLite can start from that name's rows.
- **Unplayable files:** stored with `read_error` (left out of queries, not retried until changed). ffmpeg's logging is silenced, because an app has no console.
- **Test fixtures:** made with the ffmpeg command-line tool at test time, so nothing binary is committed. The tests skip without the tool.
- **`movie.nfo`:** applies to any video in its folder. The design's "shared by two videos applies to neither" was dropped (it needs the folder's other entries).

**Files**
- New:
  - `src/media/ffmpeg.{hpp,cpp}`, `src/media/query.{hpp,cpp}`
  - `include/hoardor/audio/audio.hpp`, `src/audio/{read,library}.cpp`
  - `include/hoardor/video/video.hpp`, `src/video/{sources.hpp,sources.cpp,read.cpp,library.cpp}`
  - `tests/support/media_files.hpp`, `tests/audio/{read,library}_test.cpp`, `tests/video/{sources,read,library}_test.cpp`, `tests/master/metadata_pass_test.cpp`
  - `benchmarks/audio/query_benchmark.cpp`
  - `DOCUMENTATION/application/engines/{audio,video}.md`
- Changed:
  - `CMakeLists.txt`, `tests/CMakeLists.txt` (`HOARDOR_FFMPEG_TOOL`), `benchmarks/CMakeLists.txt`, `third_party/CMakeLists.txt` (ffmpeg include folder as SYSTEM)
  - `include/hoardor/master/sync_worker.hpp`, `src/master/sync_worker.cpp`
  - `DOCUMENTATION/application/{DATABASE.md, CODE_TREE.md, CODE_TREE.html, features/media_listing.md, engines/master.md}`

**Tests:** 177 in total (45 new in phases 2–4), all passing on Linux.
- **Audio reading:** FLAC 24/96, MP3 320 CBR, AAC, Vorbis, Opus, WAV, ALAC; tags, totals, dates, sort tags; fallbacks; Unicode; embedded covers; garbage and missing files.
- **Audio queries:**
  - pending work (unsettled, offline, stale, errors)
  - filters ignoring case and articles
  - paging every order without gaps or repeats
  - album groups in every order
  - several artists or genres per track
  - two copies of an album as one group
  - cascades, unused names, bad requests
- **Video:**
  - `.nfo` (movie, episode, tvshow, Kodi URL tail, broken), names (SxxEyy, NxNN, season folders, years)
  - streams and languages, HDR10 and HLG, `.nfo` over tags, posters, attachments
  - one card per movie whatever the copies; show, season, and episode grouping; directors vs writers; removed posters; group paging
- **Metadata pass:** the end-to-end read, only changed files next time, unsettled files waiting, videos with their `.nfo` and poster, cancel.

**Performance** (Release, this VM): see `features/media_listing.md` §8a.
- Albums by name: about 3.6 ms per page at both 50k and 500k tracks.
- An album's tracks: 0.1 ms.
- Albums of a genre: 3.6 ms at 50k, 29 ms at 500k.
- Albums by date added: 73 ms at 50k, 837 ms at 500k.
- Album and track counts: 376 and 142 ms at 500k.

**Known limitations / follow-ups**
- Aggregate orders (date added, year) and counts scale with the library; a per-group summary could fix them if daily use shows lag.
- The cold-HDD reading speed is to be measured on Windows.
- Album identity is (album artist, album). Two different albums with the same name by one artist merge, and an untagged compilation splits by track artist.
- The metadata pass only runs after a sync, never on its own at startup (it would wake drives).

### Media library v1 designed (draft v2): metadata moves into this branch; `DATABASE.md` (2026-10-02, branch `abhinavp06/MEDIA_LISTING`)

**Summary:** The user reviewed draft v1's mockups (listing by file and folder names) and rejected names from files. `features/media_listing.md` is rewritten as **Media library v1**. Nothing is built yet:
- **Reading metadata:** ffmpeg reads tags and stream info, in two new engines, `audio` and `video`, right after each sync (driven by `master`), plus a backlog for existing libraries.
- **Generic queries:** filter, order, group, and count by fields, with keyset cursors.
- **Movie and show info** from offline sources only: `.nfo`, embedded tags, and names as a last resort.
- **Copies** of an album or movie in different qualities are grouped by TYLI, never by hoardor.

The user also asked for a database design document: `DATABASE.md` now describes every table of `v0.1.0`.

**Decisions** (the user, 2026-10-02; all in ARCHITECTURE's decision log)
- Metadata on this branch, before the player. This reverses the morning's order.
- ffmpeg only; TagLib rejected.
- Offline movie/show info only.
- Generic APIs, with layouts and copy grouping in TYLI. The "layout" and "listed kinds" category fields of draft v1 are dropped.
- TYLI owns thumbnails.
- Backfill existing libraries, keeping entry ids.
- No books or text in this build.
- The paging types become `core`'s first component.

**Open, awaiting the user** (feature doc §8)
- pugixml for `.nfo` parsing.
- Metadata on the sync thread.
- Audio before video.
- Stream languages stored as text.

**Resolved the same day:**
- pugixml approved (it's offline).
- Metadata on the sync thread approved.
- Audio first.
- Stream languages as text.
- The user asked for parallel syncing. It's per drive, not per folder (an HDD read twice at once thrashes), so it's proposed as file engine phase 4, the next feature after this branch.

**Files**
- `DOCUMENTATION/application/features/media_listing.md` (rewritten)
- `DOCUMENTATION/application/DATABASE.md` (new): every table, column, index, and migration of `v0.1.0`, the rules (ownership, cross-engine joins, migrations, paging), and the proposed schema
- `CLAUDE.md`: `DATABASE.md` in the layout, the reading order, the Definition of done, and living documentation
- `DOCUMENTATION/application/ARCHITECTURE.md`: six decision rows
- `DOCUMENTATION/application/engines/file.md`: §4.0 marked superseded
- `DOCUMENTATION/application/CODE_TREE.md`/`.html`: `DATABASE.md`, `media_listing.md`, and the missing `v0.1.0.md`

**Follow-ups**
- The user's review of draft v2 and the mockups. Then phase 1 (ffmpeg in the build, `added_ns`, companions).

### Media listing designed (draft, awaiting approval) (2026-10-02, branch `abhinavp06/MEDIA_LISTING`)

**Summary:** The first feature after `v0.1.0`, step 1 of the order of work. `features/media_listing.md` designs listing every category from file and folder names. The user approved folder grouping and folder art for this branch on 2026-10-02. Nothing is built yet.

**What the design adds**
- **Folders:** a `file_folders` table kept by sync.
- **Albums:** folders that directly hold listed media, with disc folders (`CD1`, `Disc 2`) merged into their parent.
- **Series:** show → season folders.
- **Natural sort keys,** computed in C++ and indexed.
- **Date added.**
- **Per-category fields:** listed kinds and layout (files / albums / series).
- **Queries:** `media`, `media_count`, `albums`, `album_count`, `folders`, `folder`, with keyset cursors.
- **Folder art:** picked by configurable names.
- **Three new settings:** `disc_folder_prefixes`, `cover_names`, `cover_any_image`.
- **Schema:** migration 2, plus a one-time C++ backfill for existing libraries that keeps entry ids.

**Decisions proposed to the user** (the doc's §6)
- Cover thumbnails cached by TYLI with Qt, not `stb_image` in hoardor.
- Backfill rather than re-syncing.
- No season or episode number parsing yet.
- Layout stored in hoardor.
- Movie titles are file names for now.

**Files**
- `DOCUMENTATION/application/features/media_listing.md` (new), `DOCUMENTATION/application/engines/file.md` (§4.0 points to it), `DOCUMENTATION/CHANGELOGS/unreleased.md`.

**Follow-ups**
- The user's review of the doc and of TYLI's mockups. Then phase 1 (schema and sync).
