#include <hoardor/video/video.hpp>

#include "support/media_files.hpp"
#include "support/temp_dir.hpp"

#include <gtest/gtest.h>

#include <fstream>

namespace fs = std::filesystem;
using namespace hoardor;
using test::make_video;
using test::quoted;

namespace {

class VideoRead : public ::testing::Test {
protected:
    void write(const fs::path& file, const std::string& text) {
        fs::create_directories(file.parent_path());
        std::ofstream(file, std::ios::binary) << text;
    }
    test::TempDir dir;
};

}

TEST_F(VideoRead, StreamsLanguagesAndTags) {
    HOARDOR_SKIP_WITHOUT_FFMPEG();
    const fs::path srt = dir.path() / "subs.srt";
    write(srt, "1\n00:00:00,000 --> 00:00:00,500\nHi\n");
    const fs::path f = dir.path() / "Movies" / "x.mkv";
    ASSERT_TRUE(make_video(f, "320x240",
                           "-map 0:v -map 1:a -map 2:s -c:v libx264 -preset ultrafast -pix_fmt yuv420p -c:a aac -c:s srt "
                           "-metadata:s:a:0 language=jpn -metadata:s:s:0 language=eng -metadata title=Arrival "
                           "-metadata DATE_RELEASED=2016 -metadata genre=Drama -metadata director=\"Denis Villeneuve\"",
                           "-i " + quoted(srt.string())));
    const auto v = video::read(f);
    ASSERT_TRUE(v.has_value()) << v.error().message;
    EXPECT_EQ(v->type, video::Type::Movie);
    EXPECT_EQ(v->width, 320);
    EXPECT_EQ(v->height, 240);
    EXPECT_EQ(v->video_codec, "h264");
    EXPECT_EQ(v->frame_rate_milli, 25000);
    EXPECT_EQ(v->hdr, "");
    EXPECT_NEAR(static_cast<double>(v->duration_ms), 1000.0, 100.0);
    ASSERT_EQ(v->audio.size(), 1u);
    EXPECT_EQ(v->audio[0].language, "jpn");
    EXPECT_EQ(v->audio[0].codec, "aac");
    EXPECT_EQ(v->audio[0].channels, 2);
    ASSERT_EQ(v->subtitles.size(), 1u);
    EXPECT_EQ(v->subtitles[0].language, "eng");
    EXPECT_EQ(v->title, "Arrival");
    EXPECT_EQ(v->source, video::Source::Tags);
    EXPECT_FALSE(v->from_name);
    EXPECT_EQ(v->year, 2016);
    EXPECT_EQ(v->genres, (std::vector<std::string>{"Drama"}));
    EXPECT_EQ(v->directors, (std::vector<std::string>{"Denis Villeneuve"}));
}

TEST_F(VideoRead, HdrIsRecognized) {
    HOARDOR_SKIP_WITHOUT_FFMPEG();
    const fs::path f = dir.path() / "hdr.mkv";
    ASSERT_TRUE(make_video(f, "64x64", "-map 0:v -c:v libx264 -preset ultrafast -pix_fmt yuv420p10le "
                                       "-color_trc smpte2084 -color_primaries bt2020 -colorspace bt2020nc"));
    EXPECT_EQ(video::read(f)->hdr, "HDR10");
    const fs::path hlg = dir.path() / "hlg.mkv";
    ASSERT_TRUE(make_video(hlg, "64x64", "-map 0:v -c:v libx264 -preset ultrafast -pix_fmt yuv420p10le -color_trc arib-std-b67"));
    EXPECT_EQ(video::read(hlg)->hdr, "HLG");
}

TEST_F(VideoRead, NfoBeatsTagsAndNames_PosterIsPicked) {
    HOARDOR_SKIP_WITHOUT_FFMPEG();
    const fs::path folder = dir.path() / "Arrival (2016)";
    const fs::path f = folder / "Arrival (2016).mkv";
    ASSERT_TRUE(make_video(f, "64x64", "-map 0:v -c:v libx264 -preset ultrafast -pix_fmt yuv420p -metadata title=WrongTitle"));
    write(folder / "Arrival (2016).nfo", "<movie><title>Arrival</title><year>2016</year><genre>Science Fiction</genre>"
                                         "<director>Denis Villeneuve</director><plot>Linguist.</plot></movie>");
    write(folder / "fanart.jpg", "x");
    write(folder / "poster.jpg", "x");
    const std::vector<fs::path> companions{folder / "Arrival (2016).nfo", folder / "fanart.jpg", folder / "poster.jpg"};
    const auto v = video::read(f, companions);
    ASSERT_TRUE(v.has_value()) << v.error().message;
    EXPECT_EQ(v->title, "Arrival");
    EXPECT_EQ(v->source, video::Source::Nfo);
    EXPECT_EQ(v->info_index, 0);
    EXPECT_EQ(v->poster_index, 2);  // poster.jpg, not fanart
    EXPECT_EQ(v->genres, (std::vector<std::string>{"Science Fiction"}));
    EXPECT_EQ(v->plot, "Linguist.");
}

TEST_F(VideoRead, EpisodeFromNamesWithTheShowsNfoAndPoster) {
    HOARDOR_SKIP_WITHOUT_FFMPEG();
    const fs::path show = dir.path() / "Dark";
    const fs::path f = show / "Season 1" / "Dark S01E03.mkv";
    ASSERT_TRUE(make_video(f, "64x64", "-map 0:v -c:v libx264 -preset ultrafast -pix_fmt yuv420p"));
    write(show / "tvshow.nfo", "<tvshow><title>Dark</title><year>2017</year><genre>Mystery</genre></tvshow>");
    write(show / "poster.jpg", "x");
    write(show / "Season 1" / "folder.jpg", "x");
    const std::vector<fs::path> companions{show / "Season 1" / "folder.jpg", show / "tvshow.nfo", show / "poster.jpg"};
    const auto v = video::read(f, companions);
    ASSERT_TRUE(v.has_value()) << v.error().message;
    EXPECT_EQ(v->type, video::Type::Episode);
    EXPECT_EQ(v->show, "Dark");
    EXPECT_EQ(v->season, 1);
    EXPECT_EQ(v->episode, 3);
    EXPECT_EQ(v->title, "Episode 3");  // nothing names the episode
    EXPECT_TRUE(v->from_name);
    EXPECT_EQ(v->genres, (std::vector<std::string>{"Mystery"}));  // from tvshow.nfo
    EXPECT_EQ(v->year, 2017);
    EXPECT_EQ(v->poster_index, 2);  // the show's poster, for the shows grid
}

TEST_F(VideoRead, MovieFromNameOnly) {
    HOARDOR_SKIP_WITHOUT_FFMPEG();
    const fs::path f = dir.path() / "Heat.1995.1080p.mkv";
    ASSERT_TRUE(make_video(f, "64x64", "-map 0:v -c:v libx264 -preset ultrafast -pix_fmt yuv420p"));
    const auto v = video::read(f);
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->title, "Heat");
    EXPECT_EQ(v->year, 1995);
    EXPECT_TRUE(v->from_name);
    EXPECT_EQ(v->source, video::Source::Name);
    EXPECT_EQ(v->poster_index, -1);
}

TEST_F(VideoRead, EmbeddedPosterAttachment) {
    HOARDOR_SKIP_WITHOUT_FFMPEG();
    const fs::path jpg = dir.path() / "cover.jpg";
    ASSERT_TRUE(test::run_ffmpeg("-f lavfi -i \"color=c=blue:s=16x16:d=1\" -frames:v 1 " + quoted(jpg.string())));
    const fs::path f = dir.path() / "with.mkv";
    ASSERT_TRUE(make_video(f, "64x64", "-map 0:v -c:v libx264 -preset ultrafast -pix_fmt yuv420p -attach " + quoted(jpg.string()) +
                                           " -metadata:s:t mimetype=image/jpeg -metadata:s:t filename=cover.jpg"));
    const auto v = video::read(f);
    ASSERT_TRUE(v.has_value()) << v.error().message;
    EXPECT_TRUE(v->has_embedded_poster);
    const auto bytes = video::embedded_poster(f);
    ASSERT_TRUE(bytes.has_value());
    EXPECT_EQ(bytes->size(), fs::file_size(jpg));
    const fs::path plain = dir.path() / "plain.mkv";
    ASSERT_TRUE(make_video(plain, "64x64", "-map 0:v -c:v libx264 -preset ultrafast -pix_fmt yuv420p"));
    EXPECT_TRUE(video::embedded_poster(plain)->empty());
}

TEST_F(VideoRead, NotAVideo) {
    HOARDOR_SKIP_WITHOUT_FFMPEG();
    const fs::path audio_only = dir.path() / "song.mkv";
    ASSERT_TRUE(test::run_ffmpeg("-f lavfi -t 1 -i anullsrc -c:a flac " + quoted(audio_only.string())));
    EXPECT_FALSE(video::read(audio_only).has_value());
    EXPECT_FALSE(video::read(dir.write("junk.mkv", 200)).has_value());
    EXPECT_FALSE(video::read(dir.path() / "missing.mkv").has_value());
}
