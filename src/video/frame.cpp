// video::grab_frame: a still from the video for a poster, when nothing else has one
// (features/posters.md §4).

#include <hoardor/video/video.hpp>

#include "media/ffmpeg.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cmath>
#include <memory>

namespace hoardor::video {

namespace {

struct CodecCloser {
    void operator()(AVCodecContext* c) const { avcodec_free_context(&c); }
};
struct PacketCloser {
    void operator()(AVPacket* p) const { av_packet_free(&p); }
};
struct FrameCloser {
    void operator()(AVFrame* f) const { av_frame_free(&f); }
};
struct ScaleCloser {
    void operator()(SwsContext* s) const { sws_freeContext(s); }
};

// Packets read after a seek before giving up on a frame (the seek lands on a keyframe before
// the position, so a frame normally comes within a few packets; frame threads add a few more).
constexpr int max_packets = 2000;
// Below this average brightness (0-255) a frame counts as nearly black: try the next position.
constexpr double dark_below = 24.0;

// The first decoded frame at or after a seek to `seconds`, or nullptr.
std::unique_ptr<AVFrame, FrameCloser> decode_at(AVFormatContext* format, AVCodecContext* codec, int stream_index,
                                                double seconds) {
    const auto target = static_cast<std::int64_t>(seconds * AV_TIME_BASE);
    if (avformat_seek_file(format, -1, INT64_MIN, target, target, 0) < 0 && seconds > 0) {
        if (av_seek_frame(format, -1, target, AVSEEK_FLAG_BACKWARD) < 0) return nullptr;
    }
    avcodec_flush_buffers(codec);
    std::unique_ptr<AVPacket, PacketCloser> packet(av_packet_alloc());
    std::unique_ptr<AVFrame, FrameCloser> frame(av_frame_alloc());
    if (!packet || !frame) return nullptr;
    bool draining = false;
    for (int read = 0; read < max_packets;) {
        const int got = avcodec_receive_frame(codec, frame.get());
        if (got == 0) return frame;
        if (got != AVERROR(EAGAIN) || draining) return nullptr;
        const int r = av_read_frame(format, packet.get());
        if (r < 0) {
            avcodec_send_packet(codec, nullptr);  // the end: drain what the decoder holds
            draining = true;
            continue;
        }
        if (packet->stream_index == stream_index) {
            ++read;
            avcodec_send_packet(codec, packet.get());
        }
        av_packet_unref(packet.get());
    }
    return nullptr;
}

double brightness(const Frame& f) {
    if (f.rgb.empty()) return 0;
    // Every 7th pixel is plenty for an average.
    std::uint64_t sum = 0, n = 0;
    for (std::size_t i = 0; i + 2 < f.rgb.size(); i += 3 * 7) {
        sum += std::to_integer<unsigned>(f.rgb[i]) + std::to_integer<unsigned>(f.rgb[i + 1]) + std::to_integer<unsigned>(f.rgb[i + 2]);
        n += 3;
    }
    return n ? double(sum) / double(n) : 0;
}

Result<Frame> to_rgb(const AVFrame& in, AVRational sample_aspect, int max_height) {
    if (in.width <= 0 || in.height <= 0) return std::unexpected(Error{"an empty frame"});
    // Anamorphic video (a DVD) is stored narrower than it's shown: widen by its pixel shape.
    double shown_width = in.width;
    if (sample_aspect.num > 0 && sample_aspect.den > 0) shown_width = in.width * av_q2d(sample_aspect);
    const int height = std::min(in.height, std::max(1, max_height));
    const int width = std::max(1, static_cast<int>(std::lround(shown_width * height / in.height)));
    std::unique_ptr<SwsContext, ScaleCloser> scale(sws_getContext(in.width, in.height, static_cast<AVPixelFormat>(in.format), width,
                                                                   height, AV_PIX_FMT_RGB24, SWS_BICUBIC, nullptr, nullptr, nullptr));
    if (!scale) return std::unexpected(Error{"can't convert this picture format"});
    Frame out;
    out.width = width;
    out.height = height;
    out.rgb.resize(static_cast<std::size_t>(width) * height * 3);
    std::uint8_t* planes[4] = {reinterpret_cast<std::uint8_t*>(out.rgb.data()), nullptr, nullptr, nullptr};
    const int strides[4] = {width * 3, 0, 0, 0};
    if (sws_scale(scale.get(), in.data, in.linesize, 0, in.height, planes, strides) != height) {
        return std::unexpected(Error{"converting the frame failed"});
    }
    return out;
}

}

Result<Frame> grab_frame(const std::filesystem::path& file, std::span<const double> at, int max_height) {
    auto media = media::Media::open(file);
    if (!media) return std::unexpected(Error{media.error()});
    AVFormatContext* format = media->context();
    const AVStream* stream = media->first_stream(AVMEDIA_TYPE_VIDEO);
    if (!stream) return std::unexpected(Error{"no video stream"});
    const AVCodec* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!decoder) return std::unexpected(Error{"no decoder for this video"});
    std::unique_ptr<AVCodecContext, CodecCloser> codec(avcodec_alloc_context3(decoder));
    if (!codec || avcodec_parameters_to_context(codec.get(), stream->codecpar) < 0) return std::unexpected(Error{"can't set up the decoder"});
    codec->thread_count = 0;  // as many as ffmpeg likes: a 4K HEVC frame decodes much faster
    if (int r = avcodec_open2(codec.get(), decoder, nullptr); r < 0) return std::unexpected(Error{media::error_text(r)});
    // Only the chosen stream's packets matter; the demuxer skips the rest cheaply.
    for (unsigned i = 0; i < format->nb_streams; ++i) {
        if (static_cast<int>(i) != stream->index) format->streams[i]->discard = AVDISCARD_ALL;
    }

    const double seconds = media->duration_ms(stream) / 1000.0;
    const std::span<const double> positions = at.empty() ? std::span<const double>(default_frame_positions) : at;
    std::optional<Frame> last;
    for (double position : positions) {
        auto decoded = decode_at(format, codec.get(), stream->index, seconds > 0 ? seconds * std::clamp(position, 0.0, 1.0) : 0);
        if (!decoded) continue;
        const AVRational shape = decoded->sample_aspect_ratio.num ? decoded->sample_aspect_ratio : stream->codecpar->sample_aspect_ratio;
        auto frame = to_rgb(*decoded, shape, max_height);
        if (!frame) continue;
        if (brightness(*frame) >= dark_below) return std::move(*frame);
        last = std::move(*frame);
        if (seconds <= 0) break;  // no length: every position is the start
    }
    if (last) return std::move(*last);
    return std::unexpected(Error{"no frame could be decoded"});
}

}
