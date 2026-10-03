# Posters for every movie and show

| | |
|---|---|
| Status | **Built** (2026-10-03) on `abhinavp06/PLAYER`; waiting for the user's Windows test with their Plex |
| Engines | `video` (new: `PlexPosters`, `grab_frame`, `Group::first_entry`); TYLI's thumbnail cache uses them |
| Asked by | The user's Windows tests (2026-10-03): most movies and shows had no poster, even after the companion fix |

## 1. Why

hoardor finds a poster in three places today (`features/media_listing.md` §4.3):
- an image next to the video (`poster`, `folder`, `cover`, `<name>.jpg`, the show's poster up the tree)
- a poster inside the video (an attached picture, a Matroska `cover` attachment)
- nothing else

The user's library is a Plex library. Plex downloads its art into its own data folder, not next to the videos, so most folders have no image at all. Better name matching can't fix that.

**The user's choice (2026-10-03):** import the posters Plex already has; for anything Plex doesn't have, a still frame from the video. Plex runs on the same PC.

## 2. The order, per movie or show

1. An image next to the video (unchanged)
2. A poster inside the video (unchanged)
3. **Plex's poster** (new)
4. **A frame from the video** (new)

## 3. Plex's posters (`video::PlexPosters`)

Still fully offline: Plex's own database and image files on the same PC, opened **read-only**. hoardor never writes to them. Plex can keep running.

**Where Plex keeps them** (Plex Media Server's data folder, "PMS"):
- `PMS/Plug-in Support/Databases/com.plexapp.plugins.library.db`, a SQLite database:
  - `media_parts.file`: a video's full path, as Plex saw it (`E:\Movies\Heat (1995)\Heat.mkv`)
  - `media_parts.media_item_id` → `media_items.metadata_item_id` → `metadata_items`
  - `metadata_items`: `metadata_type` (1 movie, 2 show, 3 season, 4 episode), `parent_id` (episode → season → show), `hash` (its bundle), `user_thumb_url` (the chosen poster)
- the image, by `user_thumb_url`:
  - `metadata://posters/<name>` → `PMS/Metadata/<Movies | TV Shows>/<hash[0]>/<hash[1:]>.bundle/Contents/_combined/posters/<name>`
  - `upload://posters/<name>` → the same bundle's `Uploads/posters/<name>`
  - `media://<path>` → `PMS/Media/localhost/<path>`
  - `http(s)://…` → nothing (never fetched)
  - if that file is missing: any file in the bundle's `Contents/_combined/posters/`

**Finding the folder** (`PlexPosters::find_folder()`): the first that holds the database, of
- `%LOCALAPPDATA%/Plex Media Server` (Windows)
- `~/Library/Application Support/Plex Media Server` (macOS)
- `/var/lib/plexmediaserver/Library/Application Support/Plex Media Server` (Linux)

Environment variables only, so no platform code. A custom Plex data location (a registry value on Windows) isn't read yet.

**Matching a video** to Plex's entry:
- `open()` copies what it needs into a temporary, indexed table on hoardor's side of the connection, in one query: per video file, a key and the poster of its movie or show. Plex's database is read once per thumbnail pass, memory stays flat (SQLite's temp store), and lookups are indexed.
- **The key:** the last two path parts, lowercased, with `/` (`heat (1995)/heat.mkv`). Plex may have seen the drive under another letter, or a share by its UNC path.
- **Several hits on the key** ("Season 1/01.mkv" in two shows, one movie on two drives): the path sharing the longest tail with ours (ignoring case and slashes); a tie is nobody's.
- **An episode** gets its show's poster (the grid shows shows), else its season's.

## 4. A frame from the video (`video::grab_frame`)

- Decodes one video frame near a position (default 15 %, then 30 % and 50 % if a frame is nearly black), scaled to at most 480 px high, as RGB.
- Uses libswscale (ffmpeg's scaler, part of the same LGPL build, so no new dependency in practice).
- **A show** uses its first episode (lowest season, then episode; specials and unknown seasons last): `Group::first_entry`. **A movie** uses one of its copies.
- The frame is 16:9; TYLI's poster cells crop it to fit (`PreserveAspectCrop`), like Plex does for videos without art.

## 5. When it runs (TYLI's thumbnail cache)

- Thumbnails are made in a background pass after a sync and after tag reading, while the drives are awake (unchanged).
- **Plex's posters** come from internal storage, so they're also looked up during tag reading.
- **Frames** read the media drive, so they're only taken once tag reading has finished (not in the 15-s passes during it), and only for online roots.
- A thumbnail is named after the video entry (`<entry>-<mtime>.jpg`) and made once.
- Not retried in the same session: a failed frame; and, in passes without frames, a video Plex had nothing for.
- **Plex's folder** is found automatically; `TYLI_PLEX_DIR` (an environment variable) points TYLI to another one.
- The player bar shows the same poster as the video's card (its movie's or show's group).

## 6. API (`include/hoardor/video/video.hpp`)

```cpp
struct Frame { int width = 0, height = 0; std::vector<std::byte> rgb; };  // RGB24, rows packed
Result<Frame> grab_frame(const std::filesystem::path& file, std::span<const double> at = default_frame_positions,
                         int max_height = 480);

class PlexPosters {
public:
    static std::optional<std::filesystem::path> find_folder();
    static Result<PlexPosters> open(const std::filesystem::path& plex_folder);
    std::optional<std::filesystem::path> poster(const std::filesystem::path& video_file);
};

struct Group { … EntryId first_entry = 0; };  // the first episode (season, episode), any copy for a movie
```

## 7. Edge cases and tests

- Plex not installed, or its database unreadable: `open` fails, TYLI goes straight to frames.
- A video Plex doesn't know, an `http` poster, or a missing image file: no Plex poster.
- Two files with the same folder and name on different drives: the full path decides; neither if both differ.
- Plex saw `E:\…`, hoardor sees `F:\…` (another letter): the key matches.
- Episodes: the show's poster; without one, the season's.
- A frame in a black intro: the next position is tried; all black: the last frame is used.
- An audio-only or broken file: `grab_frame` fails.

Tests: `tests/video/plex_test.cpp` (8, a fake Plex folder: a database with Plex's three tables and bundle files), `tests/video/frame_test.cpp` (6, generated videos), `VideoLibraryTest.AGroupsFirstEntryIsItsFirstRealEpisode`; TYLI `tst_media_models` (the order and the passes).

## 8. Open items

- **To verify on the user's Plex** (the layouts above are from Plex's known structure; newer versions may differ): if Plex's posters don't show, the user runs `plex_probe.ps1` (in the Windows zip; TYLI `tools/`), a read-only check, and shares its output.
- A setting for Plex's folder (registry location, another PC), if auto-detection isn't enough.
- A frame-grabbed poster stays when Plex later gets a poster, until its thumbnail is cleared.
- Episode thumbnails (a frame per episode) for the show page.
