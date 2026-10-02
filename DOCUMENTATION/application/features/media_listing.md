# Feature: Media library v1 (metadata + generic queries)

| | |
|---|---|
| Status | **hoardor phases 1–4 built** (2026-10-02, 177 tests); TYLI (phase 5) in progress. Design approved 2026-10-02 (draft v2, after the user's review of draft v1, the file-name listing, which was rejected) |
| Branch | `abhinavp06/MEDIA_LISTING` (one PR; pairs with TYLI's branch of the same name) |
| Ships in | `v0.2.0` |
| Engines involved | `core` (first component: the paging types), `file` (date added, companions, `.nfo` kind), **`audio`** (new: track metadata + queries), **`video`** (new: movie/episode metadata + queries), `master` (reads metadata after each sync), `db` (nothing new) |
| New dependencies | **ffmpeg** (libavformat, libavcodec, libavutil; LGPL, shared), approved 2026-10-02. **pugixml** for `.nfo` files: *needs approval* (§8 D1) |
| Schema | Proposed in §6 and in `DATABASE.md` §5 |
| TYLI side | `../tyli/DOCUMENTATION/application/features/media_listing_ui.md` (views, copies and quality, thumbnails) |

## 1. Why this changed (the user, 2026-10-02)

Draft v1 listed media by file and folder names. Reviewing its mockups, the user decided:
- **Names come from metadata, not file names.** Titles, artists, albums, years, genres, track length, bit rate, resolution, and directors.
- **Metadata moves into this branch**, before the player. This reverses the morning's order: listing, then the player, then metadata.
- **ffmpeg only** reads metadata. The player needs it anyway. TagLib was rejected as a second dependency.
- **Movie and show info comes from offline sources only:** embedded tags, `.nfo` sidecar files, images next to the files, and "Title (Year)" from names as a last resort.
- **hoardor's APIs are generic.** hoardor answers "these items, filtered by these fields, sorted or grouped by those". TYLI decides layouts: albums, artist pages, genre pages, copies of the same album or movie in different qualities. **hoardor never groups copies.**
- **TYLI owns thumbnails.** hoardor only says which image is a cover and hands over its bytes.
- **Text (books, blogs) is a separate, big build:** it's out of this feature.
- **The existing library is backfilled,** keeping entry ids. And there's now a **database design doc** (`DATABASE.md`).

## 2. What the user gets (through TYLI)

- **Music:**
  - an album grid with real names and covers (embedded art or `cover.jpg`)
  - sorted by date added (newest or oldest first), alphabetically, by artist (grouped under artist headings), or by year
  - artist, genre, and year pages
  - an album page with tracks (length and bit rate by default) and a quality picker when an album exists in several qualities (16/44.1, 24/96, …)
- **Movies:**
  - a poster grid, one card per movie however many copies exist
  - a movie page with year, genres, directors (each a link), length, and a quality picker (4K / 1080p, highest by default)
  - a director's catalog
- **Shows:** a poster grid, then a show page with season tabs and episodes (real episode titles, length, quality).
- **Every grid:** the same sort options, paged as you scroll, offline drives dimmed, never waking a drive.

**Later (TODO in the mockups):**
- viewed and progress markers, and click-to-play with the queue prompt: player (step 2)
- playlists and liked songs: their own feature
- search
- an opt-in online lookup: decided against for now

## 3. Phases

| Phase | Scope | Exit criteria |
|---|---|---|
| 1. Foundations | **ffmpeg** in the build (Linux VM, Windows CI, macOS documented); `file`: `added_ns`, `FileKind::Info` (`.nfo`), `companions(entry)`; the backfill of `added_ns` | Builds on Linux and in the Windows workflow; file tests extended |
| 2. Audio | `audio` engine: read tags and stream info with ffmpeg; `audio_*` tables; generic `tracks` / `groups` / `count` queries; embedded cover bytes | Edge cases of §7; 500k benchmark |
| 3. Video | `video` engine: probe streams; embedded tags; `.nfo` (movie, tvshow, episode); name fallback; `video_*` tables; the same query shape; posters | Edge cases of §7 |
| 4. Reading after sync | `master`: after each root sync, read metadata for new and changed entries, plus a backlog of entries never read (an upgraded library); progress, cancel, yield to the UI | Survives cancel or crash mid-way; flat memory |
| 5. TYLI | TYLI's doc: models, views, thumbnail cache | — |

Phases 2 and 3 can swap if the user wants movies first.

## 4. How metadata is read

### 4.1 When (master, phase 4)

- **After every root sync**, while the drive is awake: `master::SyncWorker` runs the metadata pass on the same thread, after the sync's report is delivered. Order:
  1. entries added or changed by that sync
  2. then the **backlog**: listed-kind entries whose metadata is missing or stale (`source_size`/`source_mtime_ns` ≠ the entry's), oldest first
- **Unsettled entries are skipped.** They're read once a later sync finds them settled.
- **Cancellable** with the sync's stop token. Already-read rows are committed in batches (`batch_max_rows` / `batch_max_milliseconds`), so a cancel or crash loses at most one batch. The backlog query finds the rest next time.
- **Progress** goes through a new `MetadataProgress` callback: done / total, the current root.
- **Removed entries** lose their metadata through `ON DELETE CASCADE` from `file_entries`.
- **Engines don't call each other:** `master` pages the entries (`file::Library`) and hands each one, with its resolved path, to `audio::read` or `video::read`. Those engines know nothing about sync.

### 4.2 Audio (`audio::read(path) → Result<TrackInfo>`)

- **ffmpeg:**
  - `avformat_open_input` reads the header and the tags. `avformat_find_stream_info` runs only when the header lacks the duration or the format, with a capped `probesize`.
  - The first audio stream gives the codec, sample rate, bit depth (`bits_per_raw_sample`, else from the sample format), channels, bit rate (stream, else container), and duration.
- **Tags** are read case-insensitively across ID3v2, Vorbis comments, MP4, and APE:

  | Field | Tags |
  |---|---|
  | title | `title` |
  | artists | `artist` |
  | album artist | `album_artist` / `albumartist` |
  | album | `album` |
  | track | `track` (`3` or `3/12`) |
  | disc | `disc` (`1/2`) |
  | date | `date` / `year` / `originaldate`, with the **year** taken from it |
  | genres | `genre` |
  | sort names | `artistsort`, `albumartistsort`, `albumsort`, when present |
- **Multi-valued artists and genres** split on `;`, the NUL separator ffmpeg uses for ID3v2.4 multi-values, and `/` only for genres.
- **Fallbacks** keep every track listable:
  - no title → the file stem, flagged `title_from_name = true`, so TYLI can mark it
  - no album → the folder name, flagged the same way
  - no album artist → the first artist → `Unknown artist`
- **Lossless** comes from the codec: FLAC, ALAC, WAV/PCM, APE, WavPack, TTA.
- **Embedded cover:** `has_embedded_cover` comes from an attached-picture stream. `audio::embedded_cover(path)` returns its bytes, used by TYLI's thumbnail cache.

### 4.3 Video (`video::read(path, companions) → Result<VideoInfo>`)

- **Streams** (ffmpeg, capped probe): duration, the main video stream's width, height, codec, frame rate, and HDR (from color transfer: PQ means HDR10, HLG, and Dolby Vision side data), plus audio and subtitle streams with their language and codec.
- **Descriptive fields,** first non-empty wins:
  1. **`.nfo` next to the file:** `<name>.nfo`, or `movie.nfo` for a movie alone in its folder, parsed with pugixml (§8 D1). It's either `<movie>` (title, year, premiered, genres, directors, plot, runtime) or `<episodedetails>` (show title, season, episode, title, aired, directors). A `tvshow.nfo` in a parent folder (up to 2 levels) gives the show's title, year, and genres.
  2. **Embedded tags:** Matroska `TITLE`, `DATE_RELEASED`, `GENRE`, `DIRECTOR`; MP4 `©nam`, `©day`, `©gen`; `show` / `season_number` / `episode_sort`.
  3. **Names, the last resort, flagged `from_name = true`:**
     - `Title (Year)` or `Title.Year.…` for movies
     - `S01E02`, `1x02`, or `Season 1/…` for episodes, with the show from the folder above the season folder
- **Movie or episode:** an `<episodedetails>` nfo, a season or episode number, or an `SxxEyy` name means an episode. Everything else in the category is a movie. TYLI decides which categories it shows as movies or as shows.
- **Posters:**
  - from companions: `poster`, `folder`, `cover`, `<name>-poster` (images); `season01-poster` for a season; `fanart` as a backdrop (later)
  - otherwise a Matroska attachment named `cover*`

### 4.4 Companions (`file::Library::companions(EntryId) → Result<std::vector<Entry>>`)

- **What:** the image, subtitle, and info (`.nfo`) entries in the **same folder** as the entry, plus `tvshow.nfo` and posters up to 2 parent folders up for video.
- **How:** read from SQLite through a range scan of `(root_id, path_key)` on the folder's prefix, so there's no new table.
- **Who uses it:** metadata reading (nfo, posters) and TYLI (folder art and, later, subtitles).
- **A new kind,** `FileKind::Info = 6`, maps `.nfo`. Migration 2 adds it to the default Movies and Shows categories' accepted kinds when they still exist.

## 5. Generic query API

The same shape in `audio` and `video`: **filter** by fields (equality, ANDed), **order** by fields, **group** by fields with aggregates, **count**, and keyset **cursors**. No layout is built in. An "album" is just a group by (album artist, album). Field names are an enum, and SQL is built from fixed fragments, never from caller strings.

```cpp
namespace hoardor::audio {

enum class Field : std::uint8_t {
    Title, Artist, AlbumArtist, Album, Genre, Year, Disc, Track,      // descriptive
    Duration, Bitrate, SampleRate, BitDepth, Codec, Lossless,         // technical
    Added, Category, Root, Entry,                                     // from the file engine (read-only join)
};
using Value = std::variant<std::int64_t, std::string>;
struct Condition { Field field; Value value; };   // equality; Artist/Genre match any of a track's values
struct Filter { std::vector<Condition> all; };
struct Order { Field field; bool descending = false; };

struct Track {
    file::EntryId entry_id = 0;
    std::string title, album, album_artist;
    std::vector<std::string> artists, genres;
    int track = 0, track_total = 0, disc = 0, disc_total = 0, year = 0;
    std::string date;                       // as tagged: "1997", "1997-05-21"
    std::int64_t duration_ms = 0;
    int bitrate_kbps = 0, sample_rate = 0, bit_depth = 0, channels = 0;
    std::string codec;                      // "flac", "mp3", "aac", "alac", …
    bool lossless = false, has_embedded_cover = false, title_from_name = false, album_from_name = false;
    file::RootId root_id = 0; bool root_online = false;
    std::int64_t added_ns = 0; std::uint64_t size = 0;
};

struct Group {
    std::vector<std::string> keys;          // display values of the group-by fields, in order
    std::uint64_t tracks = 0;
    std::int64_t duration_ms = 0;
    std::int64_t added_first_ns = 0, added_last_ns = 0;
    int year_min = 0, year_max = 0;
    file::EntryId cover_entry = 0;          // a track to take art from: one with embedded art, else the first
    bool any_online = false;
};
enum class GroupOrder : std::uint8_t { Keys, AddedLast, Year, Tracks };   // each with `descending`

Result<core::Page<Track>> tracks(const Filter&, std::span<const Order>, std::optional<core::Cursor> after = {},
                                 std::size_t limit = 200);
Result<core::Page<Group>> groups(std::span<const Field> by, const Filter&, GroupOrder order, bool descending,
                                 std::optional<core::Cursor> after = {}, std::size_t limit = 200);
Result<std::uint64_t> count(const Filter&);
Result<std::uint64_t> group_count(std::span<const Field> by, const Filter&);
Result<std::vector<std::byte>> embedded_cover(file::EntryId);   // touches the drive: thumbnails only
}
```

**Paging types** go in `hoardor::core` (`include/hoardor/core/page.hpp`), the first `core` component (ARCHITECTURE §2: built when a feature needs it; this feature's audio and video engines share it):

```cpp
namespace hoardor::core {
struct Cursor { std::string key; std::int64_t id = 0; };   // opaque: pass `next` back for the following page
template <class T> struct Page { std::vector<T> items; std::optional<Cursor> next; };   // next empty: the end
}
```

**How TYLI's screens map onto it** (examples, not hoardor concepts):

| Screen | Call |
|---|---|
| Album grid, newest first | `groups({AlbumArtist, Album}, {Category=music}, AddedLast, desc)` |
| Album grid, alphabetical | `groups({AlbumArtist, Album}, …, Keys)`, sorted on the album's sort key |
| Grouped by artist | the same with `Keys` (album artist first); TYLI draws a heading when the artist changes |
| Artist page | `groups({AlbumArtist, Album}, {AlbumArtist=X})` plus "appears on": `groups(…, {Artist=X})` |
| Genre / year page | `groups({AlbumArtist, Album}, {Genre=X})` / `{Year=Y}` |
| Album page | `tracks({AlbumArtist=X, Album=Y}, {Disc, Track, Title})`. TYLI groups copies by (disc, track) and builds the quality picker from `bit_depth`/`sample_rate`/`codec` |

**Video** has the same shape:
- `video::Field`: `Type` (movie / episode), `Title`, `Year`, `Genre`, `Director`, `Show`, `Season`, `Episode`, `Duration`, `Height`, `Hdr`, `VideoCodec`, `Added`, `Category`, `Root`, `Entry`
- an `Item` struct with the fields of §4.3 and `from_name`
- `items` / `groups` / `count` / `group_count`, and `poster(entry) → companion entry or embedded bytes`

Examples:
- **Movie grid:** `groups({Title, Year}, {Type=movie})`, one card per movie.
- **Movie page:** `items({Title=X, Year=Y})`, the copies; TYLI picks the highest `Height`/`Hdr` by default.
- **Director's catalog:** `groups({Title, Year}, {Director=X})`.
- **Shows:** `groups({Show}, {Type=episode})` → `groups({Show, Season}, {Show=X})` → `items({Show=X, Season=N}, {Episode})`.

**Sorting text:**
- Every text field also stores a **sort key**: the tag's sort name if present, else the value with a leading "The " / "A " / "An " moved to the end (the articles are a setting), ASCII-lowercased, with digit runs made natural (`2` < `10`).
- Keys and display values are both kept, so names display exactly as tagged, never lowercased.

## 6. Schema (proposed; mirrored in `DATABASE.md` §5)

```sql
-- file, migration 2
ALTER TABLE file_entries ADD COLUMN added_ns INTEGER NOT NULL DEFAULT 0;   -- backfill: mtime_ns
CREATE INDEX file_entries_added ON file_entries (added_ns, id);
-- FileKind::Info = 6; '.nfo' → info in the default extension map; 'info' added to Movies/Shows if present

-- audio, migration 1
CREATE TABLE audio_tracks (
    entry_id INTEGER PRIMARY KEY REFERENCES file_entries(id) ON DELETE CASCADE,
    source_size INTEGER NOT NULL, source_mtime_ns INTEGER NOT NULL,    -- stale when ≠ the entry's
    title TEXT NOT NULL, title_key TEXT NOT NULL, title_from_name INTEGER NOT NULL,
    album TEXT NOT NULL, album_key TEXT NOT NULL, album_from_name INTEGER NOT NULL,
    album_artist TEXT NOT NULL, album_artist_key TEXT NOT NULL,
    track INTEGER NOT NULL, track_total INTEGER NOT NULL, disc INTEGER NOT NULL, disc_total INTEGER NOT NULL,
    date TEXT NOT NULL, year INTEGER NOT NULL,
    duration_ms INTEGER NOT NULL, bitrate_kbps INTEGER NOT NULL, sample_rate INTEGER NOT NULL,
    bit_depth INTEGER NOT NULL, channels INTEGER NOT NULL, codec TEXT NOT NULL, lossless INTEGER NOT NULL,
    has_embedded_cover INTEGER NOT NULL, read_error TEXT NOT NULL DEFAULT ''   -- unreadable: kept, so it isn't retried every sync
);
CREATE INDEX audio_tracks_album ON audio_tracks (album_artist_key, album_key, disc, track);
CREATE INDEX audio_tracks_album_title ON audio_tracks (album_key, album_artist_key);
CREATE INDEX audio_tracks_year ON audio_tracks (year, album_artist_key, album_key);
CREATE TABLE audio_names (id INTEGER PRIMARY KEY, kind INTEGER NOT NULL,      -- 1 artist, 2 genre
                          name TEXT NOT NULL, key TEXT NOT NULL, UNIQUE (kind, name));
CREATE TABLE audio_track_names (entry_id INTEGER NOT NULL REFERENCES audio_tracks(entry_id) ON DELETE CASCADE,
                                name_id INTEGER NOT NULL REFERENCES audio_names(id),
                                position INTEGER NOT NULL, PRIMARY KEY (entry_id, name_id)) WITHOUT ROWID;
CREATE INDEX audio_track_names_name ON audio_track_names (name_id, entry_id);

-- video, migration 1: video_items (entry_id PK → file_entries, source_size/mtime, type, title/title_key,
--   year, date, show/show_key, season, episode, episode_title, duration_ms, width, height, hdr,
--   video_codec, frame_rate, audio_streams TEXT (lang:codec lines), subtitle_streams TEXT, from_name,
--   nfo_entry, read_error), video_names (kind 1 genre, 2 person), video_item_names (entry_id, name_id,
--   role: genre / director / writer / actor, position). Indexes per §5's examples.
```

- **`audio_names`/`video_names`** hold each artist, genre, or person once. A track's or item's many values are rows in the link table, so "every album with genre X" is an index seek.
- **Unused names** are deleted at the end of a metadata pass.
- **Read-only joins:** `audio_*` and `video_*` queries join `file_entries` (`root_id`, `added_ns`, `size`, `unsettled`) and `file_roots` (`category_id`, `status`). These are the only cross-engine reads, and they're documented in `DATABASE.md` §1.
- **Final indexes:** whatever the 500k benchmark needs (§7), recorded in `DATABASE.md` when built.

## 7. Edge cases and tests

**Audio** (`tests/audio/`; fixtures generated by ffmpeg's own library in the test setup, so no binary files are committed)
- **Formats:** tags and stream info for FLAC, MP3 (ID3v2.3, ID3v2.4, ID3v1 only), M4A/AAC, ALAC, Ogg Vorbis, Opus, WAV, and APE when available.
- **Numbers:** `3/12` tracks, `1/2` discs, a missing track number.
- **Multi-values:** artists (`;` and ID3v2.4 multi-values) and genres.
- **Missing tags:** title and album fall back with their flags; album artist falls back to artist, then `Unknown artist`.
- **Hi-res:** 24-bit/96 kHz, 16/44.1, MP3 320 CBR and VBR (Xing). The bit rate is right for each.
- **Unicode:** Unicode tags are kept exactly; sort keys put "The Beatles" under B and "Track 2" before "Track 10".
- **Bad files:** a corrupt file is stored with `read_error` and not retried until its size or mtime changes; a zero-byte file; a file deleted between sync and read.
- **Embedded cover:** present and absent; a 5 MB picture doesn't blow memory, since it's read only on request.
- **Queries:**
  - every filter and order
  - group paging with no gaps or repeats (pages of 7 over 1,000 groups)
  - counts match the paging
  - offline roots are flagged; a category filter only returns that category's roots
  - two copies of an album (16/44.1 and 24/96) are one group with twice the tracks: grouping copies is TYLI's job
- **Incremental:** only changed or new entries are re-read; a removed entry's rows and its unused names go away.

**Video** (`tests/video/`)
- **nfo:** movie, episode, tvshow; a broken XML falls back to tags; a `movie.nfo` shared by two videos in one folder applies to neither, falling back.
- **Names:** `Title (2014).mkv`, `Title.2014.2160p.mkv`, `S01E02`, `1x02`, `Season 1/03.mkv`.
- **Quality:** HDR10 vs SDR detection; 4K and 1080p copies of one movie are two items with the same title and year.
- **Posters:** the poster priority order; `season01-poster.jpg`.

**Backlog and master** (`tests/master/`)
- An upgraded v0.1.0 database reads every entry once.
- A cancel mid-pass resumes where it stopped.
- Unsettled entries are skipped.
- Progress counts add up.
- The UI connection reads during a pass (WAL).

**Benchmarks** (500k entries, synthetic metadata rows; the reading speed is measured separately on real files)

| What | Target |
|---|---|
| Any page of 200 groups or tracks, every order | < 10 ms |
| `count`, `group_count` | < 50 ms |
| Reading speed, warm cache, FLAC | report it (expect hundreds of files per second) |
| Reading speed, cold HDD | the user measures it on Windows |
| Memory | flat during a full backlog pass |

## 8. Decisions (the user, 2026-10-02)

| # | Question | Decision |
|---|---|---|
| D1 | **pugixml** (MIT, two files, fetched by CMake) to parse `.nfo` XML | **Approved.** It's offline: compiled into hoardor, it only reads files already on disk |
| D2 | Metadata runs right after each sync, on hoardor's sync thread, not as a separate job | **Approved** |
| D3 | Development order: audio before video | Audio first (the default; the user may swap). The user also raised **parallel syncing**, recorded below |
| D4 | Stream languages (a video's audio and subtitle tracks) stored as text lines, not tables | Text for v1. Nothing queries them yet; the player only reads them |

**Parallel sync and metadata reading (raised by the user, 2026-10-02):**
- The user wants every added folder synced in parallel. The rule from ARCHITECTURE §6 still applies: **parallel per physical drive, not per folder.** Two folders on one spinning HDD read at once make its head seek back and forth, which is slower than one after the other. Different drives can run side by side.
- This is file engine phase 4: per-volume concurrency, 1 for an HDD and more for an SSD or NAS. It needs to know which roots share a device (volume ids per OS).
- **Proposed:** its own feature right after this branch. It speeds up both sync and the metadata pass, which is the slow part on a cold HDD. To be discussed with the user then.

## 8a. As built (2026-10-02)

How the code differs from §4–§7, and what was measured.

- **Headers:** one public header per engine, `include/hoardor/audio/audio.hpp` and `include/hoardor/video/video.hpp`. `core::Cursor` / `core::Page` are in `include/hoardor/core/page.hpp`.
- **Names in the API:**
  - `GroupOrder::Values` (the design's `Keys`); `Group::values`
  - video groups have `items`, `max_height`, `any_hdr`, `poster_entry`, `embedded_poster_entry`
  - `video::Type`, `video::Source`, `video::Stream`
  - `read(path, companions)` returns `info_index` / `poster_index`, and `master` maps them to entry ids
- **Work list:** each engine's `pending(category)` (settled entries, online roots, missing or stale rows); `master` reads them in one ordered pass. That covers both "changed by this sync" and "the backlog".
- **Unplugged drive:** a failed read is **skipped** (not recorded) when the file or its root has gone, so a drive unplugged mid-pass doesn't mark its files unreadable.
- **`movie.nfo`** is used for any video in its folder. The design's "shared by two videos applies to neither" isn't implemented (it would need the folder's other entries).
- **Unplayable files:** ffmpeg guesses a format even for random bytes. A file with no sample rate or no length (audio), or no picture size (video), is "not playable" and stored as unreadable. ffmpeg's own logging is silenced: hoardor reports problems in its results.
- **The query builder** (`src/media/query.*`), shared by both engines. Measuring showed three things the design didn't foresee:
  - **SQLite treats `GROUP BY` columns as a set.** It streamed artist-then-album groups from the (album, artist) index (whichever matching index was created last), then sorted every group. Each grouping now names its index (`INDEXED BY`), but only when ordered by its values and filtered broadly (category, root, type).
  - **Cursors** are a row-value comparison (`(a, b) > (?, ?)`, an index seek) when every key sorts the same way, and an OR chain otherwise. For value-ordered groups the cursor is a `WHERE` condition, not a `HAVING`.
  - **A filter on a name** (artist, genre, director) joins one link row (`name_id = (SELECT id …)`) instead of an `EXISTS`, so SQLite starts from that name's rows.
- **Fixtures:** the tests make small real media files with the ffmpeg command-line tool at test time. They skip without it.

**Performance** (Release, this VM; `benchmarks/audio/query_benchmark.cpp`; 10 tracks per album, 20 albums per artist, 40 genres):

| Query | 50k tracks | 500k tracks | Target |
|---|---|---|---|
| Albums by name: first page / a page in the middle | 3.5 / 3.4 ms | 3.7 / 3.6 ms | < 10 ms ✓ (flat) |
| Albums of a genre, first page | 3.6 ms | 28.8 ms | < 10 ms ✗ at 500k (a genre there has 12,500 tracks) |
| One album's tracks | 0.12 ms | 0.10 ms | ✓ |
| Tracks by title, a page in the middle | 0.68 ms | 0.62 ms | ✓ |
| Albums by date added (newest first), first page | 73 ms | 837 ms | ✗: needs every album's total |
| `group_count` (albums) / `count` (tracks) | 37 / 14 ms | 376 / 142 ms | < 50 ms ✗ at 500k |
| `pending_count` when nothing is pending | 25 ms | 212 ms | (runs once per pass) |

- **At the user's size** (about 15k tracks, about 1,300 albums), the slow rows are roughly 10–25 ms.
- **Known limitation:** orders by an aggregate (date added, year, track count) and counts scale with the library. If daily use shows a lag, a per-group summary kept up to date by the metadata pass would make them a page read; TYLI can also cache counts per view.
- **Not measured on the VM:** reading speed on a cold HDD. The user measures it on Windows.

## 8b. Reading speed (2026-10-02, after the user's report that sync got slow)

- **Separate job:** metadata reading is no longer part of the sync (`engines/master.md` §1a). The sync finishes as fast as before, and reading follows as a pausable background job.
- **No cover decoding:** ffmpeg's probe, which FLAC always needs, decoded embedded cover pictures to learn their pixel format. A placeholder format now skips that.
  - 100 FLACs with a 3 MB PNG cover: 20 → 159 files/s
  - without a cover: 1,733 files/s (warm cache)
- **Still read in full:** ffmpeg's FLAC reader loads the cover's bytes, about 20 ms per file on an HDD.
- **Next:** parallel reading per drive (file engine phase 4).
- **Measuring on the user's drives:** `hoardor_read <folder>` (playground).

## 8c. Search (2026-10-02, asked for by the user)

- **The field:** `audio::Field::Search` / `video::Field::Search` is a filter only.
  - Every typed word is matched as a prefix and all are required, ignoring case and accents.
  - It combines with any other filter, order, and grouping.
- **The index:** FTS5 tables (`audio_search`, `video_search`) maintained by triggers (`DATABASE.md`).
- **TYLI's search page** groups results as albums, artists, tracks, movies, and shows, by grouping on `Category` plus the usual fields.

## 9. Not in this feature

- Text (books, blogs): a separate build.
- An online lookup (TMDB, MusicBrainz).
- Writing tags.
- Lyrics.
- ReplayGain (it comes with the player).
- Play state, viewed and progress markers, playlists, and liked songs: the player and a playlist feature will add their tables, as TODOs in TYLI's mockups.
