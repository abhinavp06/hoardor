# Video engine (`hoardor::video`)

Status: **Built** (2026-10-02) as part of Media library v1, phase 3 (`features/media_listing.md`). Its tests are in `tests/video/`.

## 1. Responsibilities

- **Reading a movie or an episode:**
  - streams through ffmpeg: resolution, codec, HDR (HDR10, HLG, Dolby Vision), frame rate, duration, and the audio and subtitle streams with their languages
  - descriptions, first non-empty wins: an `.nfo` next to the file (`<name>.nfo`, then `movie.nfo`), the show's `tvshow.nfo` up to two folders up, embedded tags, and finally the file and folder names (flagged `from_name`)
  - the poster: a companion image by name, or a poster inside the file
- **Storing the results:** `video_items`, `video_names`, `video_item_names` (`DATABASE.md`).
- **Generic queries:** the same shape as the audio engine's, by `video::Field` (Type, Title, Year, Genre, Director, Show, Season, Episode, Height, Hdr, …).

Not in scope:
- Online lookups: offline sources only (ARCHITECTURE decision log, 2026-10-02).
- Playback, chapters, and Skip Intro (later).

## 2. Principles

- **Movie or episode:** an `<episodedetails>` `.nfo`, a show or season tag, or an episode pattern in the name (`S01E02`, `1x02`, or a "Season N" folder) makes it an episode. Everything else is a movie. TYLI decides which category shows which.
- **Posters:**
  - movies: `<name>-poster`, `poster`, `folder`, `cover`, `movie` in the movie's folder
  - episodes: the show's `poster`/`folder`/`cover` first (the shows grid), then the season's
  - the poster is stored as the image's entry id, and a removed image isn't handed out (a `LEFT JOIN`)
  - **no art of its own** (2026-10-03, `features/posters.md`): Plex's poster (`PlexPosters`, Plex's database and files, read-only), else a frame (`grab_frame`). These aren't stored: the app asks when it makes thumbnails, for `Group::first_entry`
- **Copies** (1080p and 4K of one movie) are separate items with the same title and year. TYLI groups them and picks the best by default.
- **Directors, writers, and genres** are `video_names` rows linked with a role, so a writer never shows up in a director's catalog.

## 3. Current state

- **Public API:** `include/hoardor/video/video.hpp`:
  - `Type`, `Source`, `Stream`, `VideoInfo`, `read(path, companions)`, `companion_prefixes(path)`, `embedded_poster`
  - `Frame`, `grab_frame(path, at, max_height)`, `PlexPosters` (`find_folder`, `open`, `poster`)
  - `Field`, `Condition`, `Filter`, `Order`, `Item`, `Group`, `GroupOrder`, `PendingEntry`
  - `Library`: `open`, `pending`, `pending_count`, `store(…, poster_entry)`, `store_error`, `remove_unused_names`, `items`, `groups`, `count`, `group_count`, `item`
- **Internal:** `src/video/sources.hpp` (`.nfo` parsing with pugixml; name parsing), tested directly.
- **Tables:** `video_items`, `video_names`, `video_item_names` (video migration 1). Migration 3 (2026-10-03) marks every item for one more read (the companion fix).
- **Search** (since 2026-10-02): `Field::Search`, full-text over the `video_search` FTS5 table (migration 2).
