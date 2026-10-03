# Player engine (`hoardor::player`)

Status: **Phases 1–3 built** (2026-10-03, `features/player.md`): playback, the queue, per-entry state, and video frames, drawn by TYLI's `VideoView` (checked on the VNC display). Waiting for the user's Windows test (phase 5). Tests in `tests/player/`.

## 1. Responsibilities

- **Playing audio and video through libmpv** (mpv's player library, decided by the user on 2026-10-03): one playback session, with transport, volume, and audio and subtitle tracks.
- **The queue:** entry ids in order, never "an album". Files are resolved one at a time through a resolver the caller injects, so the engine never calls the file engine.
- **Gapless music:** mpv holds only the current file and the next one.
- **Per-entry state:** positions, viewed state, play counts, and likes in `player_items`; its tunables in `player_settings`.
- **Video frames:** mpv's OpenGL render API behind plain function pointers (`VideoRenderer`), drawn by the app (TYLI) on its render thread.

**Not in scope:** what an album or a season is (the caller passes entry ids), drawing controls, and opening files by itself.

## 2. Principles

- **No Qt.** The engine could run in a headless daemon (audio only).
- **Never block the caller:** mpv's commands are asynchronous, and resolving a sleeping drive happens on the player's own event thread.
- **Bounded memory:** mpv's read-ahead cache is a capped setting (64 MiB, 20 s by default). It's also the first lever against stutter during a sync on the same HDD (`features/file_sync.md` OI-1).
- **One connection per thread:** the progress writes and the resolver use a connection that only the player thread touches.

## 3. Current state

- **Public API:** `include/hoardor/player/player.hpp`:
  - `Settings`, `SettingLimits`, `ItemState`
  - `Library` (any connection, one thread): `open`, `state`, `states`, `set_liked`, `save_position`, `count_play`, `load_settings`, `save_settings`
  - `State`, `Track`, `Status`, `Resolver`, `Callbacks`, `Outputs`
  - `VideoRenderer`: `create`, `ready`, `render`, `destroy`, `set_update_callback`
  - `Player`: `start`, the queue (`play_now`, `add`, `jump`, `remove`, `clear`, `queue`, `current`), transport (`toggle`, `pause`, `resume`, `stop`, `next`, `previous`, `seek`, `seek_by`, `set_volume`, `set_muted`, `select_audio`, `select_subtitle`), `status`, `video`
- **`master::file_resolver(database_file)`** (`include/hoardor/master/playback.hpp`): the resolver on `file::Library::resolve`, with its own connection, and errors in words ("its drive is offline", "the file is missing").
- **Tables:** `player_items`, `player_settings` (player migration 1, `DATABASE.md`).
- **Dependency:** libmpv (`third_party/CMakeLists.txt`: pkg-config `mpv`, or `MPV_ROOT` on Windows).

## 4. Roadmap

| Step | Scope |
|---|---|
| First draft (`abhinavp06/PLAYER`) | queue, transport, gapless, tracks, video frames, progress and viewed |
| Later | bit-perfect output (exclusive WASAPI), ReplayGain, chapters, Skip Intro (analysis), a saved queue, `master` yielding a sync to playback (OI-1) |
