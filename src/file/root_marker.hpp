#pragma once

// The .hoardor-root marker file (engines/file.md §2.3): a small text file in a
// library root holding the root's UUID, so the root is recognized wherever the
// drive is mounted.

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace hoardor::file::detail {

inline constexpr std::string_view marker_file_name = ".hoardor-root";

// A random (version 4) UUID, e.g. "3f2b8c1e-9a4d-4e7b-8c2a-1d5e6f7a8b9c".
std::string new_uuid();

// The UUID in `directory`'s marker, or std::nullopt if there is no readable, valid marker.
std::optional<std::string> read_marker(const std::filesystem::path& directory);

// Writes (or overwrites) the marker and reads it back. false if the storage refused.
bool write_marker(const std::filesystem::path& directory, std::string_view uuid);

}
