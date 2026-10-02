#pragma once

// A thin C++ layer over ffmpeg's demuxer, shared by the audio and video engines.
// Internal: no ffmpeg type appears in hoardor's public headers.

extern "C" {
#include <libavformat/avformat.h>
}

#include <cstddef>
#include <expected>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

namespace hoardor::media {

struct FormatCloser {
    void operator()(AVFormatContext* context) const;
};

// An opened media file: its container and streams. Reading stops after the header
// (plus a short probe when the header lacks something), never the whole file.
class Media {
public:
    // probe: also run avformat_find_stream_info (bounded) when the header leaves the
    // duration or the first stream's parameters unknown.
    static std::expected<Media, std::string> open(const std::filesystem::path& file, bool probe = true);

    AVFormatContext* context() const { return context_.get(); }
    // The first stream of this type that isn't an attached picture, or nullptr.
    AVStream* first_stream(AVMediaType type) const;
    // The attached picture (embedded cover art), or nullptr.
    AVStream* attached_picture() const;
    // Duration in milliseconds: the stream's, else the container's; 0 if unknown.
    std::int64_t duration_ms(const AVStream* stream) const;

private:
    explicit Media(AVFormatContext* context) : context_(context) {}
    std::unique_ptr<AVFormatContext, FormatCloser> context_;
};

// The first non-empty value among `keys` (matched ignoring case), trimmed, or "".
std::string tag(const AVDictionary* tags, std::initializer_list<const char*> keys);
// Like tag(), also looking in the first audio/video stream's tags (Ogg and some MP4s keep tags there).
std::string any_tag(const Media& media, std::initializer_list<const char*> keys);

// "3/12" -> {3, 12}; "7" -> {7, 0}; garbage -> {0, 0}.
std::pair<int, int> number_pair(const std::string& text);
// The first plausible year (1000-2999) at the start of a date: "1997-05-21" -> 1997; else 0.
int year_of(const std::string& date);

// ffmpeg error code as text.
std::string error_text(int code);

}
