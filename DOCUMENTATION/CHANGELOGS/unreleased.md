# Unreleased

Work that is done but not yet part of a version. The newest entries come first. When a version is cut, these entries move unchanged into `v<version>.md`, and this file is emptied (see `README.md`).

### Player phase 3: video frames drawn by TYLI (2026-10-03, branch `abhinavp06/PLAYER`)

**Summary:** No engine code changed. TYLI's `VideoView` now draws mpv's frames through `player::VideoRenderer` (TYLI's changelog).
- **Checked on the VNC display:** OpenGL through Mesa llvmpipe, Qt 6.12, libmpv 0.37.
- **Noted for apps (`features/player.md` §10a):** mpv waits up to 0.2 s for every frame nobody draws.

**Files:** `DOCUMENTATION/application/{engines/player.md, features/player.md}`

### Player engine, phases 1–2: playback on libmpv, the queue, per-entry state (2026-10-03, branch `abhinavp06/PLAYER`)

**Summary:** The new engine `hoardor::player` (`features/player.md`, `engines/player.md`), built and tested headless.
- **`Player`:**
  - one libmpv session on its own thread: the queue of entry ids (`play_now`, `add`, `jump`, `remove`, `clear`)
  - transport (pause, resume, toggle, stop, next, previous with restart after 3 s, seek, volume, mute) and audio/subtitle track selection
  - status callbacks, and errors for files that can't play (skipped; a queue where nothing plays stops after one pass)
  - **Gapless:** mpv holds only the current file and the next, appended ahead of time, so nothing far ahead is resolved and no drive is woken early.
- **`player::Library`:** `player_items` (position, viewed, play count, last played, liked) and `player_settings`, on any connection. TYLI reads states for a page in one query (`json_each`) and sets likes on its own connection.
- **Rules:**
  - resume where you stopped (video by default, after 60 s), and viewed at 90 %
  - a play counted at 50 % or 4 min, or at the end
  - progress saved every 10 s and on pause, seek, stop, and the end; volume and mute remembered
- **`VideoRenderer`:** mpv's OpenGL render API behind plain function pointers, for TYLI's video item (phase 3).
- **`master::file_resolver`:** entry → path through `file::Library::resolve`, on a connection of its own, with errors in words.
- **Build:** libmpv in `third_party/CMakeLists.txt` (`hoardor_mpv`: pkg-config `mpv`, or `MPV_ROOT` on Windows). `CLAUDE.md` lists it.

**Decisions (as built, `features/player.md` §10a)**
- **`Library` is separate from `Player`,** so liking and reading states don't need playback.
- **mpv's commands are synchronous on the player thread,** which keeps their order. Every public method only posts to that thread, so callers never wait (tested with a resolver that takes 1.5 s).
- **The position is polled every 0.5 s,** and reaching the end counts a play. mpv sends few `time-pos` updates for audio-only files, and a 0.6 s track went uncounted before.
- **Resume:** any saved position not yet viewed. A "not in the last 5 s" rule was dropped, because short clips had nothing left to resume.

**Tests:** 210 pass (18 new).
- **`PlayerLibraryTest` (6):** zeros until played or liked (in the asked order), like/unlike/like again, viewed only turns on, play counts, settings with limits and bad values, cascade with the entry.
- **`PlayerTest` (12)**, with real libmpv (`ao=null`, `vo=null`) on generated files:
  - the queue in order with play counts; the prompt's add / clear-and-play
  - an offline file reported once and skipped; nothing playable stops; a corrupt file skipped
  - pause, seek, previous, and next; removing the next or the current item
  - a video resuming until viewed; music starting at the beginning
  - volume and mute remembered; commands returning at once while a drive spins up
  - master's resolver words (offline, missing, gone)

**Files**
- `include/hoardor/player/player.hpp`, `src/player/library.cpp`, `src/player/player.cpp` (new)
- `include/hoardor/master/playback.hpp`, `src/master/playback.cpp` (new)
- `CMakeLists.txt`, `third_party/CMakeLists.txt`, `tests/CMakeLists.txt`
- `tests/player/library_test.cpp`, `tests/player/player_test.cpp` (new)
- `CLAUDE.md`, `DOCUMENTATION/application/{DATABASE.md, CODE_TREE.md, CODE_TREE.html, engines/player.md, features/player.md}`

**Follow-ups**
- **Phase 3:** a frame drawn through `VideoRenderer` in TYLI (VNC, then Windows).
- **Sanitizers:** the suite under ASan/TSan with mpv's threads; mpv may need `tests/tsan.supp` entries.
- **A test for exactly two files in mpv's playlist** (only indirectly covered).

### Player design approved; likes join it (2026-10-03, branch `abhinavp06/PLAYER`)

**Summary:** The user approved the player design ("looks good") and asked for a like heart next to the add-to-playlist button: green when liked, empty when not.
- **Liking is stored now**, as `liked_ns` in the per-entry table, so the heart remembers.
- **The table** is renamed from `player_progress` to `player_items` (the user's state per file: position, viewed, play count, last played, liked), with a partial index on liked rows. It isn't built yet, so the rename costs nothing.
- **The API gains** `set_liked`, `state`, and `states` (replacing `progress` / `progress_of`).
- **The "liked songs" list** still comes with the playlists feature.

**Files**
- `DOCUMENTATION/application/features/player.md` (status: approved; §1, §4.2, §5, §6, §8)
- `DOCUMENTATION/application/engines/player.md`
- `DOCUMENTATION/application/DATABASE.md` (§5)

### Player designed (draft, awaiting approval) (2026-10-03, branch `abhinavp06/PLAYER`)

**Summary:** The next feature after `v0.2.0`. The user chose libmpv for playback, wrapped by a new hoardor engine `player`. The first-draft scope is the music queue with the queue prompt and a now-playing bar, a video page, and resume plus viewed markers. Bit-perfect output is a TODO.
- `features/player.md` designs the engine:
  - the queue of entry ids with an injected resolver
  - mpv holding only the current and next file (gapless without resolving ahead)
  - threads, and video frames through mpv's OpenGL render API
  - read-ahead as the first answer to OI-1
  - the progress, viewed, and play-count rules, the tables, settings, edge cases, the Windows packaging, and the license note
- `engines/player.md` is the engine's reference.

**Checked before designing (throwaway, not committed):** a small Qt 6.12 program drew libmpv 0.37 frames into a Qt Quick item on this VM's VNC display (Mesa OpenGL). Two findings:
- **Qt Quick must use OpenGL:** Windows defaults to Direct3D 11.
- **mpv's "advanced control" render mode hung** in that setup, so it stays off.

**Decisions**
- **libmpv** over Qt Multimedia, and over our own ffmpeg + miniaudio (the user, 2026-10-03). Recorded in ARCHITECTURE.
- **Proposed:** the rest of the design, pending the user's review.

**Files**
- `DOCUMENTATION/application/features/player.md` (new), `DOCUMENTATION/application/engines/player.md` (new)
- `DOCUMENTATION/application/DATABASE.md` (§5 proposed: `player_progress`, `player_settings`)
- `DOCUMENTATION/application/ARCHITECTURE.md`

