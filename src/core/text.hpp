#pragma once

// Text helpers shared by the audio and video engines (ARCHITECTURE §2: small shared
// helpers move to core once a second engine needs them). Internal, not public API.

#include <string>
#include <string_view>
#include <vector>

namespace hoardor::core {

// A key that sorts naturally with plain byte comparison, so it can be indexed:
// - ASCII letters lowercased; other UTF-8 bytes kept as they are;
// - a leading article ("the ", "a ", "an " by default) moved out of the way, so
//   "The Beatles" sorts under B;
// - each run of digits replaced by its length (one byte) and the digits without
//   leading zeros, so "2" < "10" and "Track 9" < "Track 10".
const std::vector<std::string>& default_articles();
std::string sort_key(std::string_view text, const std::vector<std::string>& articles = default_articles());

// Leading and trailing ASCII whitespace removed.
std::string trim(std::string_view text);

// Splits on any of the separator characters, trims each part, drops empty parts and
// duplicates (ASCII case-insensitively), and keeps the order.
std::vector<std::string> split_values(std::string_view text, std::string_view separators);

}
