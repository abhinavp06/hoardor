#include "file/text.hpp"

#include <cstdint>

namespace hoardor::file::detail {

namespace {

constexpr std::string_view replacement_character = "\xEF\xBF\xBD";  // U+FFFD

void append_code_point(std::string& out, char32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

}

std::string ascii_lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

std::string normalize_extension(std::string_view extension) {
    while (!extension.empty() && is_space(extension.front())) extension.remove_prefix(1);
    while (!extension.empty() && is_space(extension.back())) extension.remove_suffix(1);
    if (!extension.empty() && extension.front() == '.') extension.remove_prefix(1);
    return ascii_lower(extension);
}

Utf8 utf8_from_bytes(std::string_view bytes) {
    Utf8 result;
    result.text.reserve(bytes.size());
    std::size_t i = 0;
    while (i < bytes.size()) {
        const auto b0 = static_cast<unsigned char>(bytes[i]);
        if (b0 < 0x80) {
            result.text.push_back(static_cast<char>(b0));
            ++i;
            continue;
        }
        // Expected sequence length and the valid range of the second byte
        // (this rejects overlong forms, surrogates, and code points above U+10FFFF).
        std::size_t length = 0;
        unsigned char low = 0x80, high = 0xBF;
        if (b0 >= 0xC2 && b0 <= 0xDF) { length = 2; }
        else if (b0 == 0xE0) { length = 3; low = 0xA0; }
        else if (b0 >= 0xE1 && b0 <= 0xEC) { length = 3; }
        else if (b0 == 0xED) { length = 3; high = 0x9F; }
        else if (b0 >= 0xEE && b0 <= 0xEF) { length = 3; }
        else if (b0 == 0xF0) { length = 4; low = 0x90; }
        else if (b0 >= 0xF1 && b0 <= 0xF3) { length = 4; }
        else if (b0 == 0xF4) { length = 4; high = 0x8F; }

        bool ok = length != 0 && i + length <= bytes.size();
        for (std::size_t k = 1; ok && k < length; ++k) {
            const auto b = static_cast<unsigned char>(bytes[i + k]);
            const unsigned char lo = (k == 1) ? low : 0x80;
            const unsigned char hi = (k == 1) ? high : 0xBF;
            ok = b >= lo && b <= hi;
        }
        if (ok) {
            result.text.append(bytes.substr(i, length));
            i += length;
        } else {
            result.text.append(replacement_character);
            result.valid = false;
            ++i;
        }
    }
    return result;
}

Utf8 utf8_from_utf16(std::u16string_view utf16) {
    Utf8 result;
    result.text.reserve(utf16.size());
    for (std::size_t i = 0; i < utf16.size(); ++i) {
        const char16_t unit = utf16[i];
        if (unit >= 0xD800 && unit <= 0xDBFF) {
            if (i + 1 < utf16.size() && utf16[i + 1] >= 0xDC00 && utf16[i + 1] <= 0xDFFF) {
                const char32_t cp = 0x10000 + ((static_cast<char32_t>(unit) - 0xD800) << 10)
                                    + (static_cast<char32_t>(utf16[i + 1]) - 0xDC00);
                append_code_point(result.text, cp);
                ++i;
                continue;
            }
            result.text.append(replacement_character);
            result.valid = false;
        } else if (unit >= 0xDC00 && unit <= 0xDFFF) {
            result.text.append(replacement_character);
            result.valid = false;
        } else {
            append_code_point(result.text, unit);
        }
    }
    return result;
}

Utf8 to_utf8(const std::filesystem::path& path) {
    // path::u8string() is not used: on Windows it throws on unpaired surrogates,
    // and on Linux it returns invalid bytes unchecked.
    using Native = std::filesystem::path::value_type;
    const auto generic = path.generic_string<Native>();
    if constexpr (sizeof(Native) == sizeof(char16_t)) {
        return utf8_from_utf16(std::u16string_view(reinterpret_cast<const char16_t*>(generic.data()), generic.size()));
    } else {
        return utf8_from_bytes(std::string_view(reinterpret_cast<const char*>(generic.data()), generic.size()));
    }
}

}
