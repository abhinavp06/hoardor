#pragma once

// Helpers shared by library.cpp and library_sync.cpp. Not part of the public API.

#include <hoardor/file/library.hpp>

namespace hoardor::file::detail {

Error database_error(const db::Error& error);
bool is_constraint(const db::Error& error);
// The unique key of an entry within its root: the path itself, ASCII-lowercased on case-insensitive roots.
std::string path_key(std::string_view relative_path, bool case_sensitive);
std::string_view root_columns();
Root read_root(const db::Statement& statement);
std::string_view entry_columns();
Entry read_entry(const db::Statement& statement);
Result<void> set_root_status(db::Database& database, RootId id, RootStatus status);

}
