# Shows, specials, and extras from release layouts

Status: **Built** (2026-10-12, branch `abhinavp06/PLAYLISTS`). Engines: `video` (names, `Library::attach`), `master` (calls it). TYLI side: `features/media_listing_ui.md` §7h.

## 1. Why

The user's Windows test (2026-10-12): "some of the TV shows are not showing up, even on a rescan". A video was an episode only when an `.nfo`, a tag, or its name said so, and names were matched by two patterns (`S01E02`, `1x02`) plus "a number in a folder named exactly `Season N`". Everything else became a movie, and a Shows category lists only episodes, so those shows vanished. A rescan can't help: it re-reads only changed files, and the same name gives the same answer.

Checked against the user's real folder lists (`tyli/tools/list_tree.ps1`, 2026-10-12):

| | Videos | Before | After |
|---|---|---|---|
| Shows | 1,773 | 967 episodes, 806 "movies": Evangelion, Dragon Ball, and The Bads of Bollywood missing entirely; 499 featurettes and DBZ's films, OVAs, and specials as invisible movies; shows named after the release folder ("Breaking Bad (2008) Season 1-5 S01-S05 (1080p BluRay …)") | 18 shows, 1,149 episodes, 52 specials, 572 extras |
| Movies | 520 | 520 "movies": 199 films plus 321 extras and samples as their own cards ("Trailer" with 15 copies) | 189 cards (198 films), 316 extras on their films, 6 samples skipped |

The user's decisions (2026-10-12): featurettes and other extras belong to their show's or movie's page as **Extras**, not in the grids; films, OVAs, and specials inside a show's folder are that show's **Specials** (season 0); **samples** are skipped.

## 2. Names (`video::detail::from_name(file, root)`, `src/video/sources.cpp`)

`read(file, companions, root)` now passes the library folder, so only the folders between it and the file are read (a root called "Extras" or "Featurettes" means nothing).

Order of the checks:
1. **Sample:** a folder named `Sample`/`Samples`, or a name that is `sample` or starts or ends with it (`sample-Revenge.2017…`, `drive.2011.sample`). `read()` returns `sample_error` without opening the file; master stores it as unreadable (never listed, never read again) and counts it as skipped.
2. **Extra:** below an extras folder that isn't the top folder: `Featurettes`, `Extras` (also `1b. Extras`, `Featurettes 5.1`), `Bonus …`, `Special Features`, `Behind the Scenes`, `Deleted Scenes`, `Interviews`, `Trailers`, `Teasers`, `Making Of`, or exactly `Other`, `Scenes`, `Shorts`. The outermost such folder counts.
   - title: the cleaned name, prefixed with the folder it's in when that says something (`Deleted Scenes - Bingo`; not `Featurettes 5.1`)
   - season: a season folder between the extras folder and the file (`Featurettes/Season 2/…`)
   - show: a guess, the cleaned name of the folder above the extras folder, with its year (`Library::attach` replaces it)
   - an extras folder wins over `.nfo`, tags, and names
3. **Episode**, by the first match:
   - `S01E02`, `s01.e02`, `S01 - E02`, `S01 E001`, `S2005E01` (seasons up to 4 digits), `1x02`; a double episode's `-E03` isn't part of the title
   - `Episode 12 Title`, `Ep 12`, unless the name has a year in brackets (`Star Wars Episode 4 (1977)` stays a movie)
   - `E01` as a word (`The Bads Of Bollywood E03`)
   - anime: `[Group] Show - 01`, `Show - 1001 [1080p]`, `Show - 05v2` (a number after ` - ` at the end, only bracketed tags after it; a 4-digit year doesn't count)
   - in a season folder: `03 - Title`, `E03`, `Episode 3`
   - season: the name's, else the file's season folder, else 1
4. **Movie**, otherwise: the year from `(2014)`, `[1968]`, `(1989 BluRay - 1080p)`, or a dotted `Title.2014.2160p`; a name without a year takes its folder's (`Nekromantik.2.1991…/creep-nekromantik21080.mkv`). `[group]` tags are dropped.

**Season folders** (`season_of_folder`): the exact forms as before (`Season 1`, `S01`, `Series 2`, `Specials`), and now also with more text (`Season 1 (1999-2000)`, `Succession (2018) S03`, `Show (2021) S01 (1080p …)`, `Specials (1995-2014)`), but never a range (`Season 1-6`, `S01-S06`, which name a whole show's folder).

**Show names** (`show_name`): `[group]` tags dropped, then cut at the first of: a year in brackets or dotted (kept as the year), a season word (`S01`, `Season 1-5`, `Specials`), a release word (`1080p`, `BluRay`, `WEB-DL`, `x265`, `HEVC`, `10bit`, …), or `(The) Complete`. An episode's show is the name before its number (`Breaking Bad (2008) - S01E01` → "Breaking Bad"); with nothing there, the nearest folder that isn't only a season. When the file's spelling is all lowercase and a folder has the same name, the folder's spelling wins (`adolescence.s01e01` → "Adolescence").

**Titles**: release words and what follows them are cut, and a bracket the cut left open goes too (`Pilot (1080p BluRay x265 Silence)` → "Pilot", `The Fog of Courage (2014 - 720p)` → "The Fog of Courage"); a trailing checksum (`[113C492A]`) goes; dots between digits stay (`Evangelion 3.33`).

## 3. What a file can't know alone: `video::Library::attach(root)`

After master reads a root's videos (if it stored any), it runs `attach(root)` in its own short transaction. It reads the database only.

- **Specials:** a movie whose type came from its name (not an `.nfo`), in a folder below a show's own folder, becomes an episode of season 0 of that show, number 0, its title without the show's name in front (`Dragon Ball Z - Movie 01 - Dead Zone` → `Movie 01 - Dead Zone`). "A show's own folder" means an ancestor (nearest first, never the root itself) whose `show_name` equals, by sort key, the show of an episode below it. So `DBZ/DRAGON BALL Z (1989-2019) - …/2a. Movies/…` joins Dragon Ball Z, while `Krzysztof Kieslowski/Blue (1993)/` next to `Krzysztof Kieslowski/Dekalog/` stays a movie.
- **Extras:** the folder above the extras folder decides: the most common show among the episodes below it (not specials), else the largest movie file there (its title and year). With neither, the guess from the name stays. So a movie's extras carry the movie's real title even when it came from tags or an `.nfo` (`Amélie`).
- Lookups are range scans of `file_entries (root_id, path_key)` below a folder (`"<folder>/"` up to `"<folder>0"`), a few per candidate. Idempotent: a second pass changes nothing.

## 4. API and schema

- `video::Type::Extra = 3`. For an extra, `show` (and `Field::Show`) is the show or movie it belongs to, `year` the movie's year (0 for a show), `season` the season folder it was in (else -1).
- `read(file, companions, root = {})`; `sample_error`.
- `Library::attach(RootId) -> Result<std::uint64_t>` (how many changed).
- Queries: the grids already filter `Type` (1 movies, 2 episodes), so extras never show there. A show's extras: `{Type=3, Show=X}`; a movie's: `{Type=3, Show=title, Year=year}`.
- **Video migration 4:** `UPDATE video_items SET source_size = -1`, so every video is read once more with the new rules (as migration 3 did). Rows stay listed until re-read.
- `master::MetadataReport::skipped` also counts samples.

## 5. Tests

- `tests/video/sources_test.cpp`: `Names.EpisodesFromReleaseLayouts` (the user's 11 layouts), `Names.NotEpisodes` (a year after " - ", "Episode 4 (1977)", a special's prime, a film in a show's folder), `Names.MovieYearFromItsFolder`, `Names.ExtrasAndSamples` (labels, seasons, owner guesses, the top-folder rule, a root named like an extras folder, four sample spellings, "Samples of Life" isn't one), `Names.LooseSeasonFoldersAndShowNames`; `Names.Episodes` updated ("Ghosts", "Fargo").
- `tests/video/library_test.cpp`: `AttachMakesSpecialsOfFilmsInAShowsOwnFolder` (DBZ, an `.nfo` movie left alone, the director's-folder case, idempotent), `AttachNamesExtrasAfterTheirShowOrMovie` (a show, a tagged movie title, the largest file, no owner), `MigrationFourReadsEveryVideoAgain`.
- `tests/master/metadata_pass_test.cpp`: `ShowsGetTheirEpisodesSpecialsAndExtrasAndSamplesAreSkipped` (a real sync: counts, the grid, a special, an extra, a sample never opened).
- By hand (2026-10-12): the user's 2,293 show and movie paths mirrored on the VM (hard links to one tiny video) and synced through `SyncWorker`: 2,287 read, 0 failed, 6 samples skipped, the results in §1.

## 6. Open items

- **TYLI's Extras section** on the show and movie pages: designed with a mockup first (TYLI `features/media_listing_ui.md` §7h). Until then extras are stored and queryable but not shown.
- **The folder rule** (TODO E): a show is its folder, whatever the names say. This feature still decides by names, then ties things together by folders in `attach`.
- Bare episode titles (`Pilot.mkv`) and date-named shows (`Show 2024-01-15`) are still movies, unless they sit in a show's folder (then specials).
- Metadata editing (TYLI `PRODUCT.md` I-14) will let the user fix what names get wrong.
