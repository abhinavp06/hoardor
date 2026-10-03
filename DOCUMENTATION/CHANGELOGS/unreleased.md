# Unreleased

Work that is done but not yet part of a version. The newest entries come first. When a version is cut, these entries move unchanged into `v<version>.md`, and this file is emptied (see `README.md`).

### Playlists and liked songs: the engine (2026-10-03, branch `abhinavp06/PLAYLISTS`)

**Summary:** The engine side of the user's next feature. Scope (the user): music only, manual playlists, and Liked Songs. From the design review: no "add a whole album", the user's own order with pinned playlists first. Design: `features/playlists.md`.

**What changed:**
- **Player migration 3:**
  - `player_playlists`: name, a unique `name_key`, `pinned`, `sort`, times.
  - `player_playlist_items`: playlist, entry, position, added, with an order index and an entry index.
  - Both cascade: deleting a playlist removes its rows, and a file that leaves the library leaves its playlists.
- **On `player::Library`:**
  - **Playlists:** `playlists` (pinned first, then the user's order, each with its track count and length) and `playlist`; `create_playlist` (trimmed; unique ignoring ASCII case; new ones on top of the unpinned), `rename_playlist`, `delete_playlist`, `set_pinned` (to the top of its new group), `move_playlist` (within its group).
  - **Rows:** `add_to_playlist` (in order; skips what's there; refuses non-tracks; counts both), `remove_from_playlist`, `move_in_playlist` (renumbers), `playlist_items` (keyset pages), `playlist_entries`, `playlists_with`.
  - **Liked Songs:** `liked` (tracks only, newest first, keyset pages), `liked_count`, `liked_entries`.
- **The audio join:** "music only" is a read-only join on `audio_tracks` (`read_error = ''`), like the video engine joining the file engine's tables.

**Decisions:**
- No duplicates by default.
- Positions may have gaps; a move renumbers the whole playlist.
- The order index isn't `UNIQUE`, because SQLite checks uniqueness row by row during a renumbering.
- Names fold ASCII case only.

**Tests:** 240 pass (+7 `PlaylistTest`): names (trim, case, taken, empty, rename to its own name), order (new on top, pinning, moving within a group, past the end), adds (order, skips, refusals, counts, duration), rows (move to top, middle, past the end; remove, ignoring other playlists' rows), paging, the cascade, and Liked Songs. Clean under AddressSanitizer.
- **Fixed on the way:** `PlayerLibraryTest.MigrationTwo…` now drops migration 3's tables to simulate a version-1 database.
- **Found on the way:** a test helper looped over `value()` of a temporary `std::expected`, a dangling reference that AddressSanitizer caught. Fixed by keeping the value.

**Files:**
- `include/hoardor/player/player.hpp`, `src/player/{library.cpp, playlists.cpp (new)}`
- `CMakeLists.txt`, `tests/CMakeLists.txt`, `tests/player/{playlist_test.cpp (new), library_test.cpp}`
- `DOCUMENTATION/application/{features/playlists.md, engines/player.md, DATABASE.md, CODE_TREE.md, CODE_TREE.html}`

**Follow-ups:** moved or renamed files drop out of playlists until move detection exists; an option to allow duplicates.
