# Unreleased

Work that is done but not yet part of a version. The newest entries come first. When a version is cut, these entries move unchanged into `v<version>.md`, and this file is emptied (see `README.md`).

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

