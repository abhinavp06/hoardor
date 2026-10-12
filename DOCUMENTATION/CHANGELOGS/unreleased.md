# Unreleased

Work that is done but not yet part of a version. The newest entries come first. When a version is cut, these entries move unchanged into `v<version>.md`, and this file is emptied (see `README.md`).

### Shows, specials, and extras from release layouts (2026-10-12, branch `abhinavp06/PLAYLISTS`)

**Summary:** The user's Windows test: "some of the TV shows are not showing up, even on a rescan". A video was an episode only through an `.nfo`, a tag, `S01E02`/`1x02` in its name, or a number in a folder named exactly `Season N`. Everything else became a movie, and a Shows category lists only episodes. Checked against the user's real folder lists (TYLI `tools/list_tree.ps1`): 806 of 1,773 videos in Shows were "movies". Evangelion (`[Anime Time] Neon Genesis Evangelion - 01`), Dragon Ball (`Episode 1 Title`, no season folder), and The Bads of Bollywood (`Show E01`) were missing entirely. 499 featurettes, DBZ's films, OVAs, and specials, and Evangelion's films were invisible movies. Shows that did appear were often named after their release folder ("Breaking Bad (2008) Season 1-5 S01-S05 (1080p BluRay x265 …)"). In Movies, 321 of 520 videos were extras and samples shown as films ("Trailer" was one card with 15 copies). Design: `features/shows_and_extras.md`.

**What changed:**
- **Names** (`video::detail`, `src/video/sources.cpp`):
  - more episode shapes: `S01 - E02`, `S2005E01`, `Episode 12 Title` (not with a year in brackets), `E01`, anime `[Group] Show - 01` / `Show - 1001 [1080p]` (not a year); a double episode's `-E02` is dropped from the title; season 1 when nothing says
  - season folders with more text (`Season 1 (1999-2000)`, `Succession (2018) S03`, `Specials (1995-2014)`), never a range (`Season 1-6`, `S01-S06`)
  - `show_name`: `[group]` tags, release words (1080p, BluRay, x265, …), season words, and "(The) Complete" cut; the year kept. An episode's show is the name before its number, else its nearest show-like folder; the folder's spelling wins over an all-lowercase file name
  - titles: release words cut, plus a bracket the cut left open and a trailing checksum; `[group]` tags dropped; dots between digits kept (`Evangelion 3.33`); a movie named without a year takes its folder's
  - **extras:** below an extras folder that isn't the top folder (`Featurettes`, `Extras`, `1b. Extras`, `Deleted Scenes`, `Behind the Scenes`, `Trailers`, `Interviews`, `Other`, …), titled `<subfolder> - <name>`, with the season folder it's in and a guess at its owner
  - **samples:** a `Sample` folder or a name that is or ends in `sample`
  - only the folders below the library folder count: `from_name(file, root)`
- **Public API** (`video.hpp`):
  - `Type::Extra = 3`: its `show` is the show or movie it belongs to, and `year` the movie's year
  - `read(file, companions, root = {})`
  - `sample_error`: read() returns it without opening the file
  - `Library::attach(root)`:
    - a movie named only by its file, below a show's own folder (an ancestor whose cleaned name is the show of an episode below it), becomes a special: season 0, number 0, without the show's name in front of its title
    - an extra takes the show of the episodes under the folder above its extras folder, else the largest movie there (its real title and year)
    - it reads the database only, through range scans of `file_entries (root_id, path_key)`
- **master:** reads videos with their root, stores a sample as unreadable and counts it as `skipped` (never opened, never read again), and runs `attach(root)` in a short transaction after a root's videos are read.
- **Video migration 4:** every video is read once more (`source_size = -1`, like migration 3). Rows stay listed meanwhile.

**Decisions (the user, 2026-10-12):**
- Featurettes and the like are **extras on their show's or movie's page**, not cards.
- Films, OVAs, and specials inside a show's folder are its **Specials** (season 0).
- **Samples** are skipped.
- Rejected:
  - "Every video in a Shows category is an episode": hoardor doesn't know what a category holds, and that's the folder rule's job (TYLI TODO E).
  - Tying specials to a show by the top folder: the user's `DBZ/` holds two shows.
  - Tying them by any episode in a parent folder: a director's folder with a mini-series would swallow the director's films (`Krzysztof Kieslowski/Dekalog/` next to `Krzysztof Kieslowski/Blue (1993)/`). The ancestor's own name has to be the show's.
  - `N of M` and `Part N` as episodes: they turned DBZ's OVA parts into a bogus show.

**Tests:** 249 pass (+9).
- `Names`:
  - `EpisodesFromReleaseLayouts`: 11 of the user's real layouts
  - `NotEpisodes`
  - `MovieYearFromItsFolder`
  - `ExtrasAndSamples`: labels, seasons, owner guesses, the top folder never counts, a root named like an extras folder, sample spellings, "Samples of Life" isn't one
  - `LooseSeasonFoldersAndShowNames`
  - `Names.Episodes` now expects "Ghosts" (release words dropped) and "Fargo" (the folder's spelling)
- `VideoLibraryTest`:
  - `AttachMakesSpecialsOfFilmsInAShowsOwnFolder`: the `.nfo` movie and the director's folder stay movies; a second pass changes nothing
  - `AttachNamesExtrasAfterTheirShowOrMovie`: a tagged movie title wins over the folder's; the largest file; no owner keeps the guess
  - `MigrationFourReadsEveryVideoAgain`
- `MetadataPassTest.ShowsGetTheirEpisodesSpecialsAndExtrasAndSamplesAreSkipped`: a real sync; a junk "sample" file is never opened.
- **By hand:** the user's 2,293 show and movie paths mirrored on the VM (hard links to one tiny video) and synced through `SyncWorker` in 5.7 s:
  - 2,287 read, 0 failed, 6 samples skipped
  - Shows: all 18 shows, 1,149 episodes plus 52 specials, 572 extras on their shows
  - Movies: 189 cards, 316 extras on their films
- Clean under AddressSanitizer.

**Performance:** names cost a few `std::regex` matches per file (the read itself dominates). `attach` costs a few indexed range lookups per movie and extra, once per root after a read pass: the 2,293-file mirror's whole pass took 5.7 s, the reads included.

**Files:**
- `include/hoardor/video/video.hpp`, `include/hoardor/master/sync_worker.hpp` (a comment)
- `src/video/{sources.hpp, sources.cpp, read.cpp, library.cpp}`, `src/master/sync_worker.cpp`
- `tests/video/{sources_test.cpp, library_test.cpp}`, `tests/master/metadata_pass_test.cpp`
- `DOCUMENTATION/application/{features/shows_and_extras.md (new), engines/video.md, engines/master.md, ARCHITECTURE.md, DATABASE.md, CODE_TREE.md, CODE_TREE.html}`

**Known limitations and follow-ups:**
- TYLI doesn't show extras yet: the Extras section on show and movie pages needs a mockup first.
- Bare titles (`Pilot.mkv`) and date-named shows are still movies outside a show's folder.
- Evangelion's director's-cut episodes (`- 21'`) become specials, not alternate copies.
- The folder rule (a show is its folder) is still TODO E; metadata editing (TYLI `PRODUCT.md` I-14) will let the user fix the rest by hand.

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
