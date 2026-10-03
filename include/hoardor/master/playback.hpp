#pragma once

// What master adds to playback (features/player.md §4.3): the player engine never calls the
// file engine, so master hands it a resolver built on file::Library::resolve.

#include <hoardor/player/player.hpp>

#include <filesystem>

namespace hoardor::master {

// Resolves entries through a file::Library connection of its own, opened on first use. The
// player calls it on its own thread only, so the connection never leaves that thread.
// Errors say why in words the app can show: the drive is offline, or the file is missing.
player::Resolver file_resolver(const std::filesystem::path& database_file);

}
