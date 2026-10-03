# Audio engine (`hoardor::audio`)

Status: **Built** (2026-10-02) as part of Media library v1, phase 2 (`features/media_listing.md`). Its tests are in `tests/audio/`, and its benchmarks in `benchmarks/audio/query_benchmark.cpp`.

## 1. Responsibilities

- **Reading an audio file:** tags and stream information through ffmpeg. `read(path)` returns a `TrackInfo`, and `embedded_cover(path)` returns the cover's bytes.
- **Storing the results:** `audio_tracks`, `audio_names`, and `audio_track_names`, all owned by this engine (`DATABASE.md`).
- **Generic queries over every track:** filter, order, group, and count by `audio::Field`, with keyset cursors (`core::Page`). Layouts (albums, artist or genre pages) and grouping copies of the same album in different qualities are TYLI's.

Not in scope:
- Deciding *when* to read: `master` does that after each sync.
- Playback, ReplayGain, lyrics, and writing tags.

## 2. Principles

- **Engines don't call each other.** `pending(category)` lists the work: settled audio entries in online roots whose metadata is missing or stale (`source_size` / `source_mtime_ns` ≠ the entry's). `master` resolves the paths, calls `read`, and calls `store` or `store_error`.
- **Every track can be listed:**
  - a missing title falls back to the file name, a missing album to the folder name; both are flagged
  - a missing album artist falls back to the first artist, then "Unknown artist"
- **Unreadable files** get a row with `read_error`. They're left out of every query and not retried until their size or mtime changes.
- **Text is stored twice:** exactly as tagged for display, and as a `core::sort_key` for filters, order, and grouping. The key ignores ASCII case and a leading article, and sorts numbers naturally. A sort tag (`ALBUMARTISTSORT`, …) replaces the value in the key.
- **Artists and genres** can have several values per track. They're `audio_names` rows linked through `audio_track_names`, so "albums of genre X" starts from that genre's rows.
- **Browsing never touches a drive.** Only `read` and `embedded_cover` open files.

## 3. Current state

- **Public API:** `include/hoardor/audio/audio.hpp`:
  - `TrackInfo`, `read`, `embedded_cover`
  - `Field`, `Condition`, `Filter`, `Order`, `Track`, `Group`, `GroupOrder`, `PendingEntry`
  - `Library`: `open`, `pending`, `pending_count`, `store`, `store_error`, `remove_unused_names`, `tracks`, `groups`, `count`, `group_count`, `track`
- **Tables:** `audio_tracks`, `audio_names`, `audio_track_names` (audio migration 1, `DATABASE.md`).
- **Shared internals:** `src/media/` (the ffmpeg layer and the query builder, shared with `video`) and `src/core/text` (sort keys).
- **Performance** (50k tracks, Release, this VM; `features/media_listing.md` §7):
  - an album page by name anywhere in the list in about 3.5 ms
  - an album page of a genre in about 3.6 ms
  - one album's tracks in about 0.1 ms
  - the first album page by date added in about 75 ms, because it aggregates every album (a known limitation)
- **Search** (since 2026-10-02): `Field::Search`, full-text over the `audio_search` FTS5 table (migration 2).
