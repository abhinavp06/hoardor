#include "file/text.hpp"

#include <gtest/gtest.h>

using namespace hoardor::file::detail;

TEST(NormalizeExtension, StripsDotSpacesAndCase) {
    EXPECT_EQ(normalize_extension(".FLAC"), "flac");
    EXPECT_EQ(normalize_extension(" .Mp3 "), "mp3");
    EXPECT_EQ(normalize_extension("ogg"), "ogg");
    EXPECT_EQ(normalize_extension(""), "");
    EXPECT_EQ(normalize_extension("."), "");
}

TEST(AsciiLower, LeavesUtf8Untouched) {
    EXPECT_EQ(ascii_lower("ÄBC"), "Äbc");
}

TEST(Utf8FromBytes, AcceptsValidUtf8) {
    for (std::string_view s : {"plain", "caf\xC3\xA9", "\xE6\x97\xA5\xE6\x9C\xAC", "\xF0\x9F\x8E\xB5", "e\xCC\x81"}) {
        const Utf8 r = utf8_from_bytes(s);
        EXPECT_TRUE(r.valid) << s;
        EXPECT_EQ(r.text, s);
    }
}

TEST(Utf8FromBytes, RepairsInvalidSequences) {
    // Latin-1 'é', a lone continuation byte, an overlong '/', a UTF-8 encoded surrogate, a truncated sequence.
    for (std::string_view s : {"caf\xE9", "\x80", "\xC0\xAF", "\xED\xA0\x80", "\xE6\x97"}) {
        const Utf8 r = utf8_from_bytes(s);
        EXPECT_FALSE(r.valid);
        EXPECT_NE(r.text.find("\xEF\xBF\xBD"), std::string::npos);
        EXPECT_TRUE(utf8_from_bytes(r.text).valid) << "repaired text must be valid";
    }
}

TEST(Utf8FromUtf16, ConvertsPairsAndRepairsLoneSurrogates) {
    const Utf8 music = utf8_from_utf16(u"\U0001F3B5 café");
    EXPECT_TRUE(music.valid);
    EXPECT_EQ(music.text, "\xF0\x9F\x8E\xB5 caf\xC3\xA9");

    const std::u16string lone_high{u'a', char16_t(0xD800), u'b'};
    const Utf8 r1 = utf8_from_utf16(lone_high);
    EXPECT_FALSE(r1.valid);
    EXPECT_EQ(r1.text, "a\xEF\xBF\xBD" "b");

    const std::u16string lone_low{char16_t(0xDC00)};
    EXPECT_FALSE(utf8_from_utf16(lone_low).valid);
}

TEST(ToUtf8, UsesForwardSlashes) {
    const std::filesystem::path p = std::filesystem::path("a") / "b" / "c.mp3";
    EXPECT_EQ(to_utf8(p).text, "a/b/c.mp3");
}
