# Player engine (`hoardor::player`)

Status: **Designed, awaiting the user's approval** (2026-10-03), in `features/player.md`. Nothing is built yet.

## 1. Responsibilities

- **Playing audio and video through libmpv** (mpv's player library, decided by the user on 2026-10-03): one playback session, with transport, volume, and audio and subtitle tracks.
- **The queue:** entry ids in order, never "an album". Files are resolved one at a time through a resolver the caller injects, so the engine never calls the file engine.
- **Gapless music:** mpv holds only the current file and the next one.
- **Progress:** positions, viewed state, and play counts in `player_progress`; its tunables in `player_settings`.
- **Video frames:** mpv's OpenGL render API behind plain function pointers (`VideoRenderer`), drawn by the app (TYLI) on its render thread.

**Not in scope:** what an album or a season is (the caller passes entry ids), drawing controls, and opening files by itself.

## 2. Principles

- **No Qt.** The engine could run in a headless daemon (audio only).
- **Never block the caller:** mpv's commands are asynchronous, and resolving a sleeping drive happens on the player's own event thread.
- **Bounded memory:** mpv's read-ahead cache is a capped setting (64 MiB, 20 s by default). It's also the first lever against stutter during a sync on the same HDD (`features/file_sync.md` OI-1).
- **One connection per thread:** the progress writes and the resolver use a connection that only the player thread touches.

## 3. Current state

Nothing is built yet. The proposed API, tables, settings, and tests are in `features/player.md` §4–§8.

## 4. Roadmap

| Step | Scope |
|---|---|
| First draft (`abhinavp06/PLAYER`) | queue, transport, gapless, tracks, video frames, progress and viewed |
| Later | bit-perfect output (exclusive WASAPI), ReplayGain, chapters, Skip Intro (analysis), a saved queue, `master` yielding a sync to playback (OI-1) |
