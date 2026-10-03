#include <hoardor/video/video.hpp>

#include "support/media_files.hpp"
#include "support/temp_dir.hpp"

#include <gtest/gtest.h>

namespace fs = std::filesystem;
using namespace hoardor;
using test::quoted;
using test::run_ffmpeg;

namespace {

class VideoFrame : public ::testing::Test {
protected:
    // A video of `seconds` from a lavfi source, with a keyframe every 5 frames so seeks land close.
    fs::path make(const std::string& name, const std::string& source, double seconds, const std::string& filters = "") {
        const fs::path f = dir.path() / name;
        fs::create_directories(f.parent_path());
        run_ffmpeg("-f lavfi -t " + std::to_string(seconds) + " -i " + quoted(source) + (filters.empty() ? "" : " -vf " + quoted(filters)) +
                   " -c:v libx264 -preset ultrafast -g 5 -pix_fmt yuv420p " + quoted(f.string()));
        return f;
    }

    static double mean(const video::Frame& f) {
        double sum = 0;
        for (std::byte b : f.rgb) sum += std::to_integer<unsigned>(b);
        return f.rgb.empty() ? 0 : sum / static_cast<double>(f.rgb.size());
    }

    test::TempDir dir;
};

}

TEST_F(VideoFrame, AScaledStillKeepsTheShape) {
    HOARDOR_SKIP_WITHOUT_FFMPEG();
    const fs::path f = make("Heat.mkv", "testsrc=size=640x360:rate=25", 2);
    const auto frame = video::grab_frame(f, video::default_frame_positions, 180);
    ASSERT_TRUE(frame.has_value()) << frame.error().message;
    EXPECT_EQ(frame->width, 320);
    EXPECT_EQ(frame->height, 180);
    EXPECT_EQ(frame->rgb.size(), 320u * 180u * 3u);
    EXPECT_GT(mean(*frame), 40.0);
}

TEST_F(VideoFrame, NeverScaledUp) {
    HOARDOR_SKIP_WITHOUT_FFMPEG();
    const fs::path f = make("small.mkv", "testsrc=size=160x90:rate=25", 1);
    const auto frame = video::grab_frame(f);
    ASSERT_TRUE(frame.has_value()) << frame.error().message;
    EXPECT_EQ(frame->height, 90);
    EXPECT_EQ(frame->width, 160);
}

TEST_F(VideoFrame, ADarkIntroIsSkipped) {
    HOARDOR_SKIP_WITHOUT_FFMPEG();
    // Black until 1.5 s of 4: 15 % (0.6 s) and 30 % (1.2 s) are dark, 50 % (2 s) is white.
    const fs::path f = make("intro.mkv", "color=c=black:s=320x180:r=25", 4, "drawbox=c=white:t=fill:enable='gte(t,1.5)'");
    const auto frame = video::grab_frame(f);
    ASSERT_TRUE(frame.has_value()) << frame.error().message;
    EXPECT_GT(mean(*frame), 200.0);
}

TEST_F(VideoFrame, AllDarkStillGivesTheLastFrame) {
    HOARDOR_SKIP_WITHOUT_FFMPEG();
    const fs::path f = make("night.mkv", "color=c=black:s=320x180:r=25", 2);
    const auto frame = video::grab_frame(f);
    ASSERT_TRUE(frame.has_value()) << frame.error().message;
    EXPECT_LT(mean(*frame), 24.0);
}

TEST_F(VideoFrame, AnamorphicVideoIsWidened) {
    HOARDOR_SKIP_WITHOUT_FFMPEG();
    // A DVD-style picture: stored 320x240, shown 4:3 wider (pixel shape 4/3).
    const fs::path f = make("dvd.mkv", "testsrc=size=320x240:rate=25", 1, "setsar=4/3");
    const auto frame = video::grab_frame(f);
    ASSERT_TRUE(frame.has_value()) << frame.error().message;
    EXPECT_EQ(frame->height, 240);
    EXPECT_EQ(frame->width, 427);
}

TEST_F(VideoFrame, AudioOnlyOrMissingFilesFail) {
    HOARDOR_SKIP_WITHOUT_FFMPEG();
    const fs::path song = dir.path() / "song.flac";
    ASSERT_TRUE(test::make_media(song, {}));
    EXPECT_FALSE(video::grab_frame(song).has_value());
    EXPECT_FALSE(video::grab_frame(dir.path() / "gone.mkv").has_value());
}
