#pragma once

// Internal text helpers for the file engine. They stay private until a second
// engine needs them (then they move to core, see ARCHITECTURE §2).

#include <filesystem>
#include <string>
#include <string_view>

namespace hoardor::file::detail {

// Lowercases ASCII letters only. Non-ASCII bytes (UTF-8 sequences) are left as is.
std::string ascii_lower(std::string_view text);

// "FLAC", ".flac", " .Flac" -> "flac". Returns an empty string for an empty extension.
std::string normalize_extension(std::string_view extension);

struct Utf8 {
    std::string text;   // always valid UTF-8; invalid input is replaced with U+FFFD
    bool valid = true;  // false when the input had to be repaired
};

// Converts a path to UTF-8 with '/' separators, without ever throwing.
// On Windows the native form is UTF-16 (an unpaired surrogate is invalid).
// On Linux and macOS it is bytes (any non-UTF-8 sequence is invalid).
Utf8 to_utf8(const std::filesystem::path& path);

// The building blocks of to_utf8, exposed so both code paths can be tested on any OS.
Utf8 utf8_from_utf16(std::u16string_view utf16);
Utf8 utf8_from_bytes(std::string_view bytes);

}
