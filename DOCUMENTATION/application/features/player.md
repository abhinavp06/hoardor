# Feature: Player, first draft (audio + video)

| | |
|---|---|
| Status | **Draft, awaiting the user's approval** (2026-10-03) |
| Branch | `abhinavp06/PLAYER` (one PR; pairs with TYLI's branch of the same name) |
| Ships in | `v0.3.0` |
| Engines involved | **`player`** (new: playback through libmpv, the queue, progress and viewed state), `file` (`resolve(entry)`, already there), `master` (nothing in the first draft; yielding a sync to playback is OI-1, later), `db` (nothing new) |
| New dependency | **libmpv** (mpv's player library, client API 2.x): **approved by the user, 2026-10-03** ("libmpv in hoardor") |
| Schema | Proposed in §6 and in `DATABASE.md` §5 |
| TYLI side | `../tyli/DOCUMENTATION/application/features/player_ui.md` (the now-playing bar, the queue prompt, the video page, markers) |

## 1. Decisions so far (the user, 2026-10-03)

- **libmpv plays everything,** wrapped by a new hoardor engine `player` (no Qt). The alternatives were rejected:
  - **Qt Multimedia in TYLI:** it would put playback outside hoardor, and has no gapless music, no bit-perfect output, and weaker subtitle and buffering control.
  - **Our own ffmpeg + miniaudio:** good for music, but video (rendering, HDR, subtitles) would be a big separate build.
- **First-draft scope:**
  - music: the queue with the queue prompt, and a now-playing bar
  - video: a player page (in the window or full screen, seek, audio and subtitle tracks)
  - resume and viewed markers for movies and episodes, and play counts for music
- **Later (TODO):** bit-perfect audio (Windows exclusive mode), ReplayGain, chapters and Skip Intro, a saved queue across restarts, yielding a sync to playback (OI-1), and playlists and liked songs (their own feature).
- **Unchanged rule (2026-10-02):** the queue holds **entry ids**, never "an album". Copies found later never change what's playing or queued.

## 2. What the user gets (through TYLI)

- **Music:**
  - clicking a track opens the queue prompt: cancel, add to queue, or clear the queue and play from this track; album and artist play buttons work the same way
  - a now-playing bar: cover, title and artist, previous, play/pause, next, a seek line with times, and volume
  - gapless playback between tracks
- **Movies and episodes:**
  - play opens the video page: the picture fills the content area or the screen, with controls that hide while watching (seek, pause, audio track, subtitles, full screen)
  - playback resumes where you stopped (with "from the start" available)
- **Markers:** a viewed check, and a progress line inside the name of a movie or episode you're partway through.
- **Never frozen:** a sleeping drive takes 5–10 s to spin up. The UI shows "loading" meanwhile, and the controls stay responsive.

## 3. Phases

| Phase | Scope | Exit criteria |
|---|---|---|
| 1. Engine | libmpv in the build (Linux; Windows in the workflow); `player::Player`: load, transport, volume, track lists, events; the queue of entry ids with an injected resolver; gapless next | Headless tests with real files (`ao=null`, `vo=null`): transport, queue order, prompt actions, a missing file, an offline root, gapless handover |
| 2. Progress | `player_progress` and `player_settings` tables (player migration 1), the save cadence, viewed and play-count rules, resume | Tests: thresholds, resume, flat writes |
| 3. Video rendering | `player::VideoRenderer` (mpv's OpenGL render API behind plain function pointers); TYLI draws it in a Qt Quick item | A frame on the VNC display, and in the Windows build |
| 4. TYLI | TYLI's doc: the bridge, the now-playing bar, the queue prompt, the video page, markers | — |
| 5. Windows and OI-1 | libmpv in the Windows zip; the user's test; measure playback during a sync on the same HDD | The user's check; OI-1 numbers in the changelog |

## 4. The `player` engine

### 4.1 Responsibilities

- **One playback session:** an mpv handle, configured for an embedded player: no `mpv.conf`, no key bindings, no on-screen controller, `ytdl` off.
- **The queue:** an ordered list of entry ids and a current position. The prompt's actions: play now (clear and play), add to the end, play next (later).
- **Transport:** play, pause, toggle, stop, seek (absolute or relative), next, previous (previous restarts the track after 3 s, a setting), volume, and mute.
- **Tracks:** the audio and subtitle tracks of the playing file (embedded, plus external `.srt`/`.ass` next to it, which mpv loads with `sub-auto=fuzzy`), and choosing one.
- **State to the caller:** what's playing, paused or loading, position and duration, the track lists, errors, and the queue's changes. Callbacks run on the player's thread; TYLI forwards them to its UI thread, as it does for `SyncWorker`.
- **Progress:** saving the position, viewed, and play counts in its own tables.
- **Video frames:** handing them to whoever draws them (§4.5).

**Not in scope:**
- Deciding what an "album" is: TYLI passes the entry ids in order.
- Opening the database to find paths: the resolver is injected (§4.3).

### 4.2 Public API (sketch; `include/hoardor/player/player.hpp`)

```cpp
namespace hoardor::player {

using EntryId = std::int64_t;

enum class State : std::uint8_t { Idle, Loading, Playing, Paused };

struct Track {                 // one audio or subtitle track of the playing file
    int id;                    // mpv's track id
    std::string language, title, codec;
    bool external = false;     // a .srt/.ass next to the file
    bool selected = false;
};

struct Status {
    State state = State::Idle;
    std::optional<EntryId> entry;          // what's loaded
    std::size_t index = 0;                 // its position in the queue
    std::int64_t position_ms = 0, duration_ms = 0;
    int volume = 100;                      // 0–100
    bool muted = false;
    bool has_video = false;
    std::vector<Track> audio, subtitles;
};

// Turns an entry into an openable path. The caller builds it from file::Library::resolve,
// which can touch the drive: it's only called on the player's thread, one entry at a time.
using Resolver = std::function<Result<std::filesystem::path>(EntryId)>;

struct Callbacks {
    std::function<void(const Status&)> status;           // state, tracks, or volume changed
    std::function<void(std::int64_t position_ms)> position;  // a few times a second while playing
    std::function<void()> queue;                          // the queue changed
    std::function<void(EntryId, const Error&)> error;     // a file couldn't play: skipped
};

class Player {
public:
    static Result<Player> open(db::Database& progress_db, Resolver resolver, Callbacks callbacks,
                               const Settings& settings = Settings::defaults());

    // The queue
    void play_now(std::vector<EntryId> entries, std::size_t start = 0);   // clear and play
    void add(std::vector<EntryId> entries);                               // append
    std::vector<EntryId> queue() const;
    std::size_t current() const;
    void jump(std::size_t index);
    void remove(std::size_t index);
    void clear();

    // Transport
    void toggle(); void pause(); void resume();
    void stop();
    void next(); void previous();
    void seek(std::int64_t position_ms);
    void seek_by(std::int64_t delta_ms);
    void set_volume(int volume); void set_muted(bool muted);
    void select_audio(int track_id); void select_subtitle(std::optional<int> track_id);  // nullopt: off

    Status status() const;
    VideoRenderer* video();   // §4.5
};

}
```

`Player` is a class because it holds state (the mpv handle, the queue, its thread), which is the "class only when something holds state" rule.

### 4.3 Threads and the resolver

- **libmpv runs its own threads** (demuxer, decoders, audio output). Commands are thread-safe and asynchronous (`mpv_command_async`), so `Player`'s methods return at once and never block the caller's thread.
- **One event thread per `Player`:** it waits on `mpv_wait_event` and turns mpv's events (file loaded, end of file, property changes) into `Status` and the callbacks.
- **Resolving on the event thread:** when an item becomes current, or is next in line, the resolver runs there. A sleeping drive's 5–10 s spin-up delays only that thread, while the state says `Loading`.
- **Offline or missing:** the resolver's error (`RootOffline`, `FileMissing`) goes to the `error` callback, and the queue moves on to the next item. A queue where nothing can play stops, rather than spinning through every item.

### 4.4 Gapless, and what mpv holds

- **mpv's own playlist holds at most two files:** the current one and the next. When a file starts playing, the next queue item is resolved and appended (`loadfile <path> append`), so mpv can open it ahead of time (`prefetch-playlist=yes`) and join the two without a gap (`gapless-audio=weak`).
- **The queue itself** (any length) stays in `Player` as entry ids. Nothing is resolved ahead, so a 2,000-track queue never wakes 2,000 files' drives.
- **Jumping, removing, or reordering** rebuilds mpv's two-file playlist from the queue.

### 4.5 Video frames (`VideoRenderer`)

- **mpv's render API:** mpv draws each frame into an OpenGL framebuffer that the caller owns. hoardor wraps it without Qt types:
  - `create(get_proc_address)`: a function pointer that looks up OpenGL functions
  - `render(framebuffer_id, width, height)`
  - `set_update_callback(fn)`: mpv calls it when a new frame is ready
- **TYLI calls these on its render thread** (a Qt Quick item, TYLI's doc).
- **Qt Quick must draw with OpenGL** for this. Windows defaults to Direct3D 11, so TYLI sets the graphics API to OpenGL at startup.
- **Checked on this VM, 2026-10-03:** a throwaway program drew mpv frames into a Qt Quick item on the VNC display (Mesa llvmpipe), with Qt 6.12 and libmpv 0.37. mpv's "advanced control" mode hung in that setup, so it stays off.
- **Hardware decoding:** `hwdec=auto-safe` (the GPU decodes when it can, otherwise the CPU does).

### 4.6 Read-ahead (the cold-HDD concern, `features/file_sync.md` OI-1)

- **mpv reads ahead into a cache:** `demuxer-max-bytes` (default **64 MiB**) and `demuxer-readahead-secs` (default **20 s**), both settings.
- **Why these numbers:** 20 s of a 1080p remux (about 4 MB/s) is about 80 MB, roughly the size of the cache. A typical encode fits many times over. Memory stays bounded: the cache is a cap, not a fixed allocation.
- **The question it answers:** "a few seconds buffered" (OI-1's first lever) is enough to ride out a cold sync on the same HDD. The second lever (`master` pausing a sync of the playing drive) waits for the measurement in phase 5.

## 5. Progress, viewed, and play counts

| Rule | Default (setting) |
|---|---|
| Save the position while playing | every **10 s** (`progress_save_seconds`), and on pause, stop, seek, and the end |
| Resume from the saved position | when it's more than **60 s** in (`resume_min_seconds`) and the item isn't viewed |
| A movie or episode is **viewed** | at **90 %** of its length (`viewed_percent`), or at its end |
| A track counts as **played** | at **50 %** of its length or after **4 min**, whichever is first (`play_count_percent`, `play_count_seconds`) |
| Music resumes? | no: a track always starts at the beginning (`resume_audio`, default off) |

- **Writes stay small:** one upsert per save, at most one every 10 s.
- **Keyed by entry id:** a moved or renamed file keeps its progress, as long as the file engine keeps the entry. Move detection is file engine phase 3.
- **Generic reads for TYLI:** `progress(entry)`, and `progress_of(entries)` for a page of items. Browsing never touches a drive.

## 6. Schema (proposed; mirrored in `DATABASE.md` §5)

```sql
-- player, migration 1
CREATE TABLE player_progress (
    entry_id INTEGER PRIMARY KEY REFERENCES file_entries(id) ON DELETE CASCADE,
    position_ms INTEGER NOT NULL DEFAULT 0,
    duration_ms INTEGER NOT NULL DEFAULT 0,
    viewed INTEGER NOT NULL DEFAULT 0,           -- 1 once past viewed_percent or the end
    play_count INTEGER NOT NULL DEFAULT 0,
    last_played_ns INTEGER NOT NULL DEFAULT 0    -- Unix ns
);
CREATE INDEX player_progress_recent ON player_progress (last_played_ns);
CREATE TABLE player_settings (key TEXT PRIMARY KEY, value TEXT NOT NULL);
```

- `player_progress_recent` serves "continue watching" and "recently played" (the home page, later).
- The volume and mute state live in `player_settings`, so they survive a restart.

## 7. `player::Settings` (one struct, defaults in code, persisted in `player_settings`)

| Setting | Default | Meaning |
|---|---|---|
| `read_ahead_mib` | 64 | mpv's `demuxer-max-bytes` |
| `read_ahead_seconds` | 20 | mpv's `demuxer-readahead-secs` |
| `hardware_decoding` | true | `hwdec=auto-safe`, or `no` |
| `audio_languages` | `""` | preferred audio languages, e.g. `jpn,eng` (mpv `alang`) |
| `subtitle_languages` | `""` | preferred subtitle languages (mpv `slang`) |
| `subtitles_on` | false | show subtitles by default |
| `previous_restarts_after_seconds` | 3 | "previous" restarts the track when it's past this |
| `progress_save_seconds` | 10 | |
| `resume_min_seconds` | 60 | |
| `viewed_percent` | 90 | |
| `play_count_percent` / `play_count_seconds` | 50 / 240 | |
| `resume_audio` | false | |
| `volume` / `muted` | 100 / false | remembered |

## 8. Edge cases and tests

| Case | Expected | Test |
|---|---|---|
| The drive is offline when an item comes up | `error` (offline), skipped; the rest of the queue plays | resolver returns `RootOffline` |
| The file was deleted since the last sync | `error` (missing), skipped | resolver returns `FileMissing` |
| The file is corrupt, or not media | mpv's end-of-file with an error: `error`, skipped | a junk `.flac` |
| Nothing in the queue can play | stops (`Idle`) after one pass, with no loop | a queue of three missing files |
| A sleeping drive (slow resolve) | `Loading` until it's ready; commands still return at once | a resolver that sleeps 2 s |
| Gapless | the next track starts without a stop; only two files in mpv | two generated tracks; mpv's playlist count |
| Clear and play during loading | the old load is dropped; the new item plays | |
| Seek past the end / before the start | clamped | |
| Previous at 0:01 / at 0:30 | the previous track / restart this one | |
| Viewed and resume thresholds | as §5 | short generated videos, with the thresholds set low |
| Progress writes | at most one per save interval while playing | count writes |
| Volume and mute | remembered across a reopen | |
| `LC_NUMERIC` | libmpv needs the C locale for numbers: set at `open` (the caller is told in the header) | |
| Shutdown while playing | progress saved; the thread joins; mpv destroyed after the renderer | |

- **Tests run headless:** `ao=null` and `vo=null`, on short files that the ffmpeg CLI generates (as in the existing media tests), and skip themselves without ffmpeg.
- **Sanitizers:** the suite runs under ASan and TSan. mpv's own threads may need entries in `tests/tsan.supp`.

## 9. Build and platforms

- **Linux:** `apt install libmpv-dev` (0.37 on Ubuntu 24.04), found with `pkg-config`.
- **Windows:**
  - **The build:** a pinned libmpv "dev" build (the common source is shinchiro's mpv-winbuild-cmake), checked by SHA-256 like ffmpeg, passed with `-DMPV_ROOT=`.
  - **MSVC import library:** the package has a MinGW one, so the workflow makes an MSVC `.lib` from the DLL's exports (`dumpbin /exports` → `.def` → `lib /def`).
  - **The zip** ships `libmpv-2.dll` with its license.
- **macOS:** `brew install mpv`, documented and not tested.
- **License:** prebuilt Windows libmpv builds are GPL, because they include GPL parts. That's fine for personal use. Distributing TYLI publicly would need either GPL terms for TYLI or an LGPL build of libmpv (`-Dgpl=false`). Recorded here so it isn't a surprise later.
- **Two ffmpegs on Windows:** libmpv contains its own ffmpeg, linked in statically, next to hoardor's shared ffmpeg DLLs (used for metadata). They don't collide.

## 10. Open items

- **OI-P1:** the cold-HDD measurement (OI-1) with this player, in phase 5.
- **OI-P2:** does forcing OpenGL in Qt Quick on Windows cost anything elsewhere (startup, fonts, the software fallback on machines without a GPU driver)? Check in the Windows build.
- **OI-P3:** libmpv's Windows package and its import library in MSVC: confirm in phase 1 in the workflow.

## 11. Not in this feature

Bit-perfect output, ReplayGain, chapters, Skip Intro, playlists and liked songs, a saved queue, casting, and streaming to other devices.
