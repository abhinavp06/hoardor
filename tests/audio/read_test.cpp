#include <hoardor/audio/audio.hpp>

#include "support/media_files.hpp"
#include "support/temp_dir.hpp"

#include <gtest/gtest.h>

namespace fs = std::filesystem;
using namespace hoardor;
using hoardor::test::make_media;
using hoardor::test::MediaSpec;

namespace {

class AudioRead : public ::testing::Test {
protected:
    test::TempDir dir;
};

}

TEST_F(AudioRead, FlacTagsAndHiResStream) {
    HOARDOR_SKIP_WITHOUT_FFMPEG();
    const fs::path f = dir.path() / "Album" / "03 Song.flac";
    ASSERT_TRUE(make_media(f, {.codec_args = "-c:a flac -sample_fmt s32",
                               .input = "anullsrc=r=96000:cl=stereo",
                               .tags = {{"title", "Daydreaming"}, {"artist", "Radiohead; Guest"}, {"album_artist", "Radiohead"},
                                        {"album", "A Moon Shaped Pool"}, {"track", "3/12"}, {"disc", "1/2"},
                                        {"date", "2016-05-08"}, {"genre", "Art Rock;Alternative"},
                                        {"albumartistsort", "Radiohead, The"}}}));
    const auto t = audio::read(f);
    ASSERT_TRUE(t.has_value()) << t.error().message;
    EXPECT_EQ(t->title, "Daydreaming");
    EXPECT_EQ(t->artists, (std::vector<std::string>{"Radiohead", "Guest"}));
    EXPECT_EQ(t->album_artist, "Radiohead");
    EXPECT_EQ(t->album, "A Moon Shaped Pool");
    EXPECT_EQ(t->track, 3);
    EXPECT_EQ(t->track_total, 12);
    EXPECT_EQ(t->disc, 1);
    EXPECT_EQ(t->disc_total, 2);
    EXPECT_EQ(t->date, "2016-05-08");
    EXPECT_EQ(t->year, 2016);
    EXPECT_EQ(t->genres, (std::vector<std::string>{"Art Rock", "Alternative"}));
    EXPECT_EQ(t->album_artist_sort, "Radiohead, The");
    EXPECT_EQ(t->codec, "flac");
    EXPECT_TRUE(t->lossless);
    EXPECT_EQ(t->sample_rate, 96000);
    EXPECT_EQ(t->bit_depth, 24);
    EXPECT_EQ(t->channels, 2);
    EXPECT_NEAR(static_cast<double>(t->duration_ms), 1000.0, 50.0);
    EXPECT_GT(t->bitrate_kbps, 0);
    EXPECT_FALSE(t->title_from_name);
    EXPECT_FALSE(t->has_embedded_cover);
}

TEST_F(AudioRead, Mp3AtConstantBitRate) {
    HOARDOR_SKIP_WITHOUT_FFMPEG();
    const fs::path f = dir.path() / "song.mp3";
    ASSERT_TRUE(make_media(f, {.codec_args = "-c:a libmp3lame -b:a 320k",
                               .seconds = 2.0,
                               .tags = {{"title", "Glass Eyes"}, {"artist", "Radiohead"}, {"album", "AMSP"}, {"track", "6"}},
                               .extra = "-id3v2_version 3"}));
    const auto t = audio::read(f);
    ASSERT_TRUE(t.has_value()) << t.error().message;
    EXPECT_EQ(t->title, "Glass Eyes");
    EXPECT_EQ(t->track, 6);
    EXPECT_EQ(t->track_total, 0);
    EXPECT_EQ(t->codec, "mp3");
    EXPECT_FALSE(t->lossless);
    EXPECT_EQ(t->bit_depth, 0);
    EXPECT_NEAR(t->bitrate_kbps, 320, 2);
    EXPECT_NEAR(static_cast<double>(t->duration_ms), 2000.0, 80.0);
    EXPECT_EQ(t->album_artist, "Radiohead");  // no album artist tag: the artist
}

TEST_F(AudioRead, M4aOggOpusAndWav) {
    HOARDOR_SKIP_WITHOUT_FFMPEG();
    const std::map<std::string, std::string> tags{{"title", "T"}, {"artist", "A"}, {"album", "B"}, {"album_artist", "AA"}};
    struct Case { const char* name; const char* args; const char* codec; bool lossless; int depth; };
    for (const Case& c : {Case{"x.m4a", "-c:a aac -b:a 128k", "aac", false, 0}, Case{"x.ogg", "-c:a libvorbis", "vorbis", false, 0},
                          Case{"x.opus", "-c:a libopus", "opus", false, 0}, Case{"x.wav", "-c:a pcm_s24le", "pcm_s24le", true, 24},
                          Case{"alac.m4a", "-c:a alac -sample_fmt s16p", "alac", true, 16}}) {
        SCOPED_TRACE(c.name);
        const fs::path f = dir.path() / c.name;
        ASSERT_TRUE(make_media(f, {.codec_args = c.args, .tags = tags}));
        const auto t = audio::read(f);
        ASSERT_TRUE(t.has_value()) << t.error().message;
        EXPECT_EQ(t->codec, c.codec);
        EXPECT_EQ(t->lossless, c.lossless);
        EXPECT_EQ(t->bit_depth, c.depth);
        if (std::string(c.name) != "x.wav") {  // RIFF INFO keeps only some tags
            EXPECT_EQ(t->title, "T");
            EXPECT_EQ(t->album, "B");
            EXPECT_EQ(t->album_artist, "AA");
        }
        EXPECT_GT(t->duration_ms, 900);
        EXPECT_GT(t->bitrate_kbps, 0);
    }
}

TEST_F(AudioRead, MissingTagsFallBackToNames) {
    HOARDOR_SKIP_WITHOUT_FFMPEG();
    const fs::path f = dir.path() / "Some Folder" / "untitled track.flac";
    ASSERT_TRUE(make_media(f, {}));
    const auto t = audio::read(f);
    ASSERT_TRUE(t.has_value()) << t.error().message;
    EXPECT_EQ(t->title, "untitled track");
    EXPECT_TRUE(t->title_from_name);
    EXPECT_EQ(t->album, "Some Folder");
    EXPECT_TRUE(t->album_from_name);
    EXPECT_TRUE(t->artists.empty());
    EXPECT_EQ(t->album_artist, "Unknown artist");
    EXPECT_EQ(t->year, 0);
}

TEST_F(AudioRead, UnicodeNamesAndTags) {
    HOARDOR_SKIP_WITHOUT_FFMPEG();
    const fs::path f = dir.path() / test::u8path("Björk") / test::u8path("Jóga.flac");
    ASSERT_TRUE(make_media(f, {.tags = {{"artist", "Björk"}, {"title", "Jóga"}}}));
    const auto t = audio::read(f);
    ASSERT_TRUE(t.has_value()) << t.error().message;
    EXPECT_EQ(t->title, "J\xC3\xB3ga");
    EXPECT_EQ(t->album, "Bj\xC3\xB6rk");  // the folder, since there's no album tag
}

TEST_F(AudioRead, EmbeddedCover) {
    HOARDOR_SKIP_WITHOUT_FFMPEG();
    const fs::path with = dir.path() / "with.flac";
    const fs::path without = dir.path() / "without.flac";
    ASSERT_TRUE(make_media(with, {.cover = true}));
    ASSERT_TRUE(make_media(without, {}));
    const auto read = audio::read(with);
    ASSERT_TRUE(read.has_value()) << read.error().message;
    EXPECT_TRUE(read->has_embedded_cover);
    const auto bytes = audio::embedded_cover(with);
    ASSERT_TRUE(bytes.has_value()) << bytes.error().message;
    ASSERT_GT(bytes->size(), 2u);
    EXPECT_EQ((*bytes)[0], std::byte{0xFF});  // JPEG
    EXPECT_EQ((*bytes)[1], std::byte{0xD8});
    const auto none = audio::embedded_cover(without);
    ASSERT_TRUE(none.has_value()) << none.error().message;
    EXPECT_TRUE(none->empty());
}

// A FLAC whose STREAMINFO says 0 total samples ("unknown length"): ffmpeg writes one when it
// adds a cover picture this way. It's still a track, with no length, not "unplayable".
TEST_F(AudioRead, UnknownLengthIsStillATrack) {
    HOARDOR_SKIP_WITHOUT_FFMPEG();
    const fs::path f = dir.path() / "unknown_length.flac";
    ASSERT_TRUE(make_media(f, {.tags = {{"title", "Still a track"}}, .cover = true}));
    const auto t = audio::read(f);
    ASSERT_TRUE(t.has_value()) << t.error().message;
    EXPECT_EQ(t->title, "Still a track");
    EXPECT_EQ(t->sample_rate, 44100);
    EXPECT_GE(t->duration_ms, 0);
}

TEST_F(AudioRead, UnreadableAndMissingFiles) {
    const fs::path garbage = dir.write("broken.flac", 300);
    EXPECT_FALSE(audio::read(garbage).has_value());
    EXPECT_FALSE(audio::read(dir.path() / "nope.flac").has_value());
    EXPECT_FALSE(audio::read(dir.write("empty.mp3", 0)).has_value());
    const auto cover = audio::embedded_cover(garbage);  // an error, or simply no picture
    EXPECT_TRUE(!cover.has_value() || cover->empty());
}
