#pragma once

// Small real media files for tests, made by the ffmpeg command-line tool at test time
// (nothing binary is committed). Tests start with HOARDOR_SKIP_WITHOUT_FFMPEG(): without
// the tool they're skipped, not failed.

#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace hoardor::test {

// Set by tests/CMakeLists.txt (find_program); empty when the tool isn't installed.
inline std::string ffmpeg_tool() { return HOARDOR_FFMPEG_TOOL; }

#define HOARDOR_SKIP_WITHOUT_FFMPEG() \
    if (::hoardor::test::ffmpeg_tool().empty()) GTEST_SKIP() << "the ffmpeg tool isn't installed"

inline std::string quoted(const std::string& text) {
    std::string out = "\"";
    for (char c : text) {
        if (c == '"' || c == '\\' || c == '$' || c == '`') out.push_back('\\');
        out.push_back(c);
    }
    return out + "\"";
}

// Runs ffmpeg with these arguments (inputs and outputs, quoted where needed).
inline bool run_ffmpeg(const std::string& arguments) {
    const std::string cmd = quoted(ffmpeg_tool()) + " -hide_banner -loglevel error -y " + arguments;
    const int rc = std::system(cmd.c_str());
    EXPECT_EQ(rc, 0) << cmd;
    return rc == 0;
}

struct MediaSpec {
    std::string codec_args = "-c:a flac";              // ffmpeg output options for the stream
    std::string input = "anullsrc=r=44100:cl=stereo";  // lavfi source
    double seconds = 1.0;
    std::map<std::string, std::string> tags;           // -metadata key=value
    bool cover = false;                                // an attached 16x16 picture
    std::string extra;                                 // anything else (e.g. "-id3v2_version 4")
};

// An audio file (creating its folders). Returns false (and reports why) if ffmpeg failed.
inline bool make_media(const std::filesystem::path& file, const MediaSpec& spec) {
    std::filesystem::create_directories(file.parent_path());
    std::string args = "-f lavfi -t " + std::to_string(spec.seconds) + " -i " + quoted(spec.input);
    if (spec.cover) args += " -f lavfi -i \"color=c=red:s=16x16:d=1\" -map 0:a -map 1:v -frames:v 1 -c:v mjpeg -disposition:v attached_pic";
    args += " " + spec.codec_args;
    for (const auto& [key, value] : spec.tags) args += " -metadata " + quoted(key + "=" + value);
    return run_ffmpeg(args + " " + spec.extra + " " + quoted(file.string()));
}

// A one-second test-pattern video with silent stereo audio. `extra_inputs` come after those
// two inputs (input 2, 3, …); `output_options` choose codecs, maps, and tags.
inline bool make_video(const std::filesystem::path& file, const std::string& size, const std::string& output_options,
                       const std::string& extra_inputs = {}) {
    std::filesystem::create_directories(file.parent_path());
    return run_ffmpeg("-f lavfi -t 1 -i " + quoted("testsrc=size=" + size + ":rate=25") +
                      " -f lavfi -t 1 -i anullsrc=r=48000:cl=stereo " + extra_inputs + " " + output_options + " " +
                      quoted(file.string()));
}

}
