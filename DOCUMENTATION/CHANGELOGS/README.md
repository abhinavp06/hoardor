# Changelogs

This folder is the detailed history of hoardor. Each version has its own file, so no single file grows without bound.

| File | Contents |
|---|---|
| `unreleased.md` | Finished work that isn't in a version yet. New entries go here, newest first |
| `v<MAJOR>.<MINOR>.<PATCH>.md` | One file per version, e.g. `v0.1.0.md`. Frozen once the version is cut |
| `baseline.md` | The project state before the changelog existed |

## Versions

| Version | Date | Highlights |
|---|---|---|
| [Unreleased](unreleased.md) | — | **File Sync v1** (becomes `v0.1.0`): streaming scanner, `hoardor::db` on SQLite, library categories and roots (`.hoardor-root` markers, relocation), Sync with offline-safety rules, background `master::SyncWorker`. Also: project docs, the doc structure (engines/features), per-version changelogs, and the `core` on-demand rule |
| [Baseline](baseline.md) | before 2026-10-01 | CMake skeleton and a naive `discover()` |

Newest version first. Add a row whenever a version is cut.

## Versioning

- **Scheme:** [Semantic Versioning](https://semver.org/). Before 1.0, versions are `0.MINOR.PATCH`:
  - **MINOR:** an engine phase or feature lands on `master`, such as file engine phase 1. The public API may change between minor versions while we're below 1.0.
  - **PATCH:** fixes, documentation-only work, or build changes, with no new features.
  - **1.0.0:** cut when the user decides the public API is stable enough for TYLI to depend on.
- **The single source of the version number** is `project(hoardor VERSION ...)` in the root `CMakeLists.txt`. The changelog file name must match it.
- **The first version is `v0.1.0`.** It's cut when this branch (`abhinavp06/FILE_SCANNER_INIT`: documentation plus file scanner v1, which is file engine phases 1 and 2) merges into `master`.

## Cutting a version

Do this in the PR that completes the version, and only when the user asks for the PR:

1. Create `v<version>.md` with a header: version, date, branch(es) and PR(s), and a short summary of the version.
2. Move every entry from `unreleased.md` into it, **unchanged**, newest first.
3. Empty `unreleased.md`, leaving only its header.
4. Add a row to the **Versions** table above.
5. Bump `project(hoardor VERSION ...)` in the root `CMakeLists.txt` if it doesn't already match.
6. Tag `v<version>` in git, but only when the user asks.

## Rules

- **History matters.** Never rewrite or delete a past entry. If something was wrong, add a correcting entry in `unreleased.md`.
- **A version file is frozen once it's cut.** Moving entries from `unreleased.md` into a version file is the only restructuring allowed.
- **Update `unreleased.md`** after every completed feature, and whenever the user asks for a PR. Be thorough: a future reader with no other context should understand the change.

## Entry template

Copy this into `unreleased.md` (newest first) and leave out any sections that don't apply.

```markdown
Entry template. Copy it under [Unreleased] and leave out any sections that don't apply.

### <Feature / change title> (YYYY-MM-DD, branch `abhinavp06/<topic>`, PR #<n>)

**Summary:** One or two sentences on what changed and why it matters.

**Added**
- ...

**Changed**
- ...

**Fixed**
- ...

**Removed**
- ...

**Design decisions and trade-offs**
- The decision, the alternatives considered, and why this one won.

**Files**
- `path/to/file`: what changed in it.

**Tests**
- What is covered, the edge cases included, and how to run them.

**Performance**
- Benchmark results against the targets, if relevant.

**Known limitations / follow-ups**
- ...
```
