# Unreleased

Work that is done but not yet part of a version. The newest entries come first. When a version is cut, these entries move unchanged into `v<version>.md`, and this file is emptied (see `README.md`).

### Media library v1 designed (draft v2): metadata moves into this branch; `DATABASE.md` (2026-10-02, branch `abhinavp06/MEDIA_LISTING`)

**Summary:** The user reviewed draft v1's mockups (listing by file and folder names) and rejected names from files. `features/media_listing.md` is rewritten as **Media library v1**. Nothing is built yet:
- **Reading metadata:** ffmpeg reads tags and stream info, in two new engines, `audio` and `video`, right after each sync (driven by `master`), plus a backlog for existing libraries.
- **Generic queries:** filter, order, group, and count by fields, with keyset cursors.
- **Movie and show info** from offline sources only: `.nfo`, embedded tags, and names as a last resort.
- **Copies** of an album or movie in different qualities are grouped by TYLI, never by hoardor.

The user also asked for a database design document: `DATABASE.md` now describes every table of `v0.1.0`.

**Decisions** (the user, 2026-10-02; all in ARCHITECTURE's decision log)
- Metadata on this branch, before the player. This reverses the morning's order.
- ffmpeg only; TagLib rejected.
- Offline movie/show info only.
- Generic APIs, with layouts and copy grouping in TYLI. The "layout" and "listed kinds" category fields of draft v1 are dropped.
- TYLI owns thumbnails.
- Backfill existing libraries, keeping entry ids.
- No books or text in this build.
- The paging types become `core`'s first component.

**Open, awaiting the user** (feature doc §8)
- pugixml for `.nfo` parsing.
- Metadata on the sync thread.
- Audio before video.
- Stream languages stored as text.

**Resolved the same day:**
- pugixml approved (it's offline).
- Metadata on the sync thread approved.
- Audio first.
- Stream languages as text.
- The user asked for parallel syncing. It's per drive, not per folder (an HDD read twice at once thrashes), so it's proposed as file engine phase 4, the next feature after this branch.

**Files**
- `DOCUMENTATION/application/features/media_listing.md` (rewritten)
- `DOCUMENTATION/application/DATABASE.md` (new): every table, column, index, and migration of `v0.1.0`, the rules (ownership, cross-engine joins, migrations, paging), and the proposed schema
- `CLAUDE.md`: `DATABASE.md` in the layout, the reading order, the Definition of done, and living documentation
- `DOCUMENTATION/application/ARCHITECTURE.md`: six decision rows
- `DOCUMENTATION/application/engines/file.md`: §4.0 marked superseded
- `DOCUMENTATION/application/CODE_TREE.md`/`.html`: `DATABASE.md`, `media_listing.md`, and the missing `v0.1.0.md`

**Follow-ups**
- The user's review of draft v2 and the mockups. Then phase 1 (ffmpeg in the build, `added_ns`, companions).

### Media listing designed (draft, awaiting approval) (2026-10-02, branch `abhinavp06/MEDIA_LISTING`)

**Summary:** The first feature after `v0.1.0`, step 1 of the order of work. `features/media_listing.md` designs listing every category from file and folder names. The user approved folder grouping and folder art for this branch on 2026-10-02. Nothing is built yet.

**What the design adds**
- **Folders:** a `file_folders` table kept by sync.
- **Albums:** folders that directly hold listed media, with disc folders (`CD1`, `Disc 2`) merged into their parent.
- **Series:** show → season folders.
- **Natural sort keys,** computed in C++ and indexed.
- **Date added.**
- **Per-category fields:** listed kinds and layout (files / albums / series).
- **Queries:** `media`, `media_count`, `albums`, `album_count`, `folders`, `folder`, with keyset cursors.
- **Folder art:** picked by configurable names.
- **Three new settings:** `disc_folder_prefixes`, `cover_names`, `cover_any_image`.
- **Schema:** migration 2, plus a one-time C++ backfill for existing libraries that keeps entry ids.

**Decisions proposed to the user** (the doc's §6)
- Cover thumbnails cached by TYLI with Qt, not `stb_image` in hoardor.
- Backfill rather than re-syncing.
- No season or episode number parsing yet.
- Layout stored in hoardor.
- Movie titles are file names for now.

**Files**
- `DOCUMENTATION/application/features/media_listing.md` (new), `DOCUMENTATION/application/engines/file.md` (§4.0 points to it), `DOCUMENTATION/CHANGELOGS/unreleased.md`.

**Follow-ups**
- The user's review of the doc and of TYLI's mockups. Then phase 1 (schema and sync).
