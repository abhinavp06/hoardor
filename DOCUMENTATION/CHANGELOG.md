# Changelog

This is the detailed history of hoardor. Entries are added after every feature implementation and before every PR. The newest entries come first. Work lands under `[Unreleased]` until a version is cut. Versions follow [Semantic Versioning](https://semver.org/). The project version lives in the root `CMakeLists.txt`.

<!--
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
-->

## [Unreleased]

### Project documentation and Claude context (2026-10-01, branch `abhinavp06/file_engine_init`)

**Summary:** Set up the documentation structure and persistent Claude context, so architecture decisions and the workflow don't have to be re-explained every session.

**Added**
- `CLAUDE.md` at the repository root. Claude Code loads it automatically every session, and it imports `DOCUMENTATION/application/ARCHITECTURE.md`. It contains:
  - the project overview and common goal
  - a **session-start reading order**: architecture, changelog, git state, engine design doc, public headers, sources, tests, playground, and finally personal notes (only when asked). Each step explains what that location represents.
  - non-negotiables
  - repository layout
  - build and test commands
  - code conventions
  - workflow
  - a **Definition of done** checklist
  - **Living documentation** rules. The changelog is critical and is updated after every feature and before every PR. Past entries are never rewritten. The architecture and engine docs are kept in sync with the code continuously.
- `DOCUMENTATION/application/ARCHITECTURE.md`. It records system-wide architecture decisions with rationale and a dated decision log:
  - hoardor vs TYLI split
  - engines and event-based communication
  - SQLite persistence model
  - memory policy
  - platform strategy
  - file engine principles
  - testing approach
- `DOCUMENTATION/application/engines/`. This is the home for per-engine design docs and PRDs.
- `DOCUMENTATION/CHANGELOG.md` (this file).

**Removed**
- The empty `DOCUMENTATION/application/claude/` and `DOCUMENTATION/application/product/` folders. Claude context has to be in a root `CLAUDE.md` to load automatically, so product and design docs now live directly under `DOCUMENTATION/application/`.

**Design decisions and trade-offs**
- **Root `CLAUDE.md` vs a nested folder:** only `CLAUDE.md` files at the repository root (or in parent directories) load automatically when a session starts. Instructions placed in `DOCUMENTATION/application/claude/` would need a manual prompt every session.
- **`CLAUDE.md` imports `ARCHITECTURE.md`:** the architecture lives in one place and loads every session, with no duplication.
- **Per-engine design docs:** `engines/<engine>.md` matches the planned agent-per-engine model. Each future engine agent gets a self-contained spec.

## Baseline (before 2026-10-01)

The project state before this changelog existed, summarized from git history:
- CMake project (`hoardor` 0.1.0, C++23) with a static library target and options for the playground, tests, and running tests after the build.
- A first file engine skeleton: `hoardor::file::discover()` walks a directory recursively and returns relative path, size, and mtime for each entry. Known issues, to be fixed in the file engine work:
  - `media_type` is ignored.
  - `file_size()` throws on directories.
  - The playground doesn't compile.
  - `playground/` and `tests/` have no build targets.
- Personal notes in `DOCUMENTATION/notes/`: the feature list and early architecture thoughts.
