# Playlists and liked songs

| | |
|---|---|
| Status | **Draft, waiting for the user's approval** (2026-10-03), with TYLI `features/playlists_ui.md` and the mockups `playlist.html`, `liked.html` |
| Branch | `abhinavp06/PLAYLISTS` (both repos; started from `abhinavp06/PLAYER` while the `v0.3.0` PRs are open) |
| Engine | `player` (it already keeps per-file likes; playlists are the user's other say in what plays) |

## 1. Goals

The user's choices (2026-10-03):
- **Music only.** Movies and shows keep their own "continue watching" through resume.
- **Manual playlists:** create, rename, delete; add tracks or albums; reorder; remove.
- **Liked songs:** a built-in list, from the likes `player_items.liked_ns` already stores.
- **Not now:** smart playlists, M3U import and export, the "Resume" section (after the user's week of use).

## 2. Principles

- **A playlist holds files (entry ids), like the queue** (decision 2026-10-02): adding an album adds the copy you chose, and copies found later never change a playlist.
- **Only tracks:** an entry is accepted only if the audio engine has read it as a track (`audio_tracks`, read-only join; like the video engine joining the file engine's tables).
- **No duplicates by default:** adding a track that's already in the playlist skips it, and says how many were skipped. A later option can allow them.
- **Order is a position number:** gaps are fine (a removed file's row goes with it). A move renumbers the playlist in one statement.
- **Names are unique,** ignoring case ("Road Trip" and "road trip" are one name), and never empty.
- **A file removed from the library leaves its playlists** (`ON DELETE CASCADE` from `file_entries`). An offline drive removes nothing (the file engine's offline rule), so its tracks stay, dimmed and skipped when played.

## 3. Schema (player migration 3)

```sql
CREATE TABLE player_playlists (
    id INTEGER PRIMARY KEY,
    name TEXT NOT NULL,
    name_key TEXT NOT NULL UNIQUE,        -- the name folded for comparison (case, accents)
    created_ns INTEGER NOT NULL,
    updated_ns INTEGER NOT NULL           -- last add, remove, move, or rename
);
CREATE TABLE player_playlist_items (
    id INTEGER PRIMARY KEY,               -- one row of a playlist (the same file twice is two rows, if ever allowed)
    playlist_id INTEGER NOT NULL REFERENCES player_playlists(id) ON DELETE CASCADE,
    entry_id INTEGER NOT NULL REFERENCES file_entries(id) ON DELETE CASCADE,
    position INTEGER NOT NULL,
    added_ns INTEGER NOT NULL
);
CREATE INDEX player_playlist_items_order ON player_playlist_items (playlist_id, position);
CREATE INDEX player_playlist_items_entry ON player_playlist_items (entry_id);
```

- **`player_playlist_items_order`** serves a playlist's pages (keyset on position) and appends (`MAX(position)`). It's not `UNIQUE`: SQLite checks uniqueness row by row during a renumbering `UPDATE`.
- **`player_playlist_items_entry`** serves "which playlists hold this track" (the add menu's checks) and the cascade.
- **Liked songs** need no table: `player_items` with its partial index `player_items_liked` (`liked_ns > 0`).

## 4. API (`include/hoardor/player/player.hpp`, on `player::Library`)

```cpp
using PlaylistId = std::int64_t;
using PlaylistItemId = std::int64_t;

struct Playlist {
    PlaylistId id = 0;
    std::string name;
    std::int64_t created_ns = 0, updated_ns = 0;
    std::uint64_t tracks = 0;
    std::int64_t duration_ms = 0;        // the tracks' lengths, as read (0 for unknown ones)
};
struct PlaylistItem {
    PlaylistItemId id = 0;
    EntryId entry = 0;
    std::int64_t position = 0, added_ns = 0;
};
struct Added { std::uint64_t added = 0, skipped = 0, not_tracks = 0; };

// Playlists, a–z by name (a person has tens or hundreds, not thousands).
Result<std::vector<Playlist>> playlists();
Result<Playlist> playlist(PlaylistId id);
Result<PlaylistId> create_playlist(std::string_view name, std::int64_t now_ns);   // "" or a taken name: an error
Result<void> rename_playlist(PlaylistId id, std::string_view name, std::int64_t now_ns);
Result<void> delete_playlist(PlaylistId id);

// Appends in the given order; tracks already in it are skipped, entries that aren't tracks refused.
Result<Added> add_to_playlist(PlaylistId id, std::span<const EntryId> entries, std::int64_t now_ns);
Result<void> remove_from_playlist(PlaylistId id, std::span<const PlaylistItemId> items, std::int64_t now_ns);
// Moves one row to `index` (0-based, in the current order), renumbering the playlist.
Result<void> move_in_playlist(PlaylistId id, PlaylistItemId item, std::size_t index, std::int64_t now_ns);

// A page of rows in order (keyset on position), and every entry in order (to play it).
Result<core::Page<PlaylistItem>> playlist_items(PlaylistId id, const std::optional<core::Cursor>& after = std::nullopt,
                                                std::size_t limit = 200);
Result<std::vector<EntryId>> playlist_entries(PlaylistId id);
// Which playlists hold this file (the add menu's checks).
Result<std::vector<PlaylistId>> playlists_with(EntryId entry);

// Liked songs: newest like first.
struct Liked { EntryId entry = 0; std::int64_t liked_ns = 0; };
Result<core::Page<Liked>> liked(const std::optional<core::Cursor>& after = std::nullopt, std::size_t limit = 200);
Result<std::uint64_t> liked_count();   // tracks only
Result<std::vector<EntryId>> liked_entries();
```

- **Track details** (title, artist, album, length) come from the audio engine: the app looks each page's entries up (`audio::Library`), as it does for the queue.
- **`liked` lists tracks only:** a liked movie stays liked, but isn't a song.

## 5. Edge cases

- Creating or renaming to an empty name, a name of only spaces, or a taken name (any case): an error naming the problem.
- Adding: an empty list does nothing; a mix of new, present, and non-track entries adds the new ones in order and counts the rest; adding to a deleted playlist is an error.
- Removing rows that aren't in that playlist: ignored. Moving past the end: to the end.
- A file deleted from the library: its rows go; positions keep their gaps; `tracks` and `duration_ms` count only what's left.
- A drive offline: nothing is removed.
- Deleting a playlist deletes its rows (cascade), and nothing else.
- Liked songs: unliking removes it from the list at the next read; liked videos aren't listed.
- Large playlists (10k rows): pages stay keyset-fast; `playlist_entries` returns 10k ids (80 KB).

## 6. Tests (`tests/player/playlist_test.cpp`)

Create, rename (case-only change allowed), and delete; empty and taken names; adding in order with skips and refusals; removing; moving to the start, the middle, and past the end; paging; `playlist_entries` order; `playlists_with`; the cascade when a file leaves the library; `tracks` and `duration_ms`; liked songs order, tracks only, and count; migration 3 on an existing database.

## 7. Open items

- **Sort order of playlists:** a–z for now; "recently played" may suit daily use better.
- **Moves and renames of files:** a moved file is a new entry (move detection isn't built), so it drops out of playlists. When file phase 3 (moves) lands, its rows should follow.
- **Allowing duplicates:** a later option if wanted.
