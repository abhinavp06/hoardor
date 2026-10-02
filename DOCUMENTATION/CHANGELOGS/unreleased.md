# Unreleased

Work that is done but not yet part of a version. The newest entries come first. When a version is cut, these entries move unchanged into `v<version>.md`, and this file is emptied (see `README.md`).

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
