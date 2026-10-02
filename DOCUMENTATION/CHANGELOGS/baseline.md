# Baseline (before 2026-10-01)

This isn't a version, only the starting point. It's the project state before the changelog existed, summarized from git history:
- CMake project (`hoardor` 0.1.0, C++23) with a static library target and options for the playground, tests, and running tests after the build.
- A first file engine skeleton: `hoardor::file::discover()` walks a directory recursively and returns relative path, size, and mtime for each entry. Known issues, to be fixed in the file engine work:
  - `media_type` is ignored.
  - `file_size()` throws on directories.
  - The playground doesn't compile.
  - `playground/` and `tests/` have no build targets.
- Personal notes in `DOCUMENTATION/notes/`: the feature list and early architecture thoughts.
