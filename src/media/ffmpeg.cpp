#include "media/ffmpeg.hpp"

#include "core/text.hpp"
#include "file/text.hpp"

#include <charconv>
#include <mutex>

extern "C" {
#include <libavutil/error.h>
#include <libavutil/log.h>
}

namespace hoardor::media {

void FormatCloser::operator()(AVFormatContext* context) const { avformat_close_input(&context); }

std::string error_text(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buffer, sizeof buffer);
    return buffer;
}

std::expected<Media, std::string> Media::open(const std::filesystem::path& file, bool probe) {
    // ffmpeg prints warnings about odd files to stderr; hoardor reports problems through its
    // results instead (an app has no console to read them in).
    static std::once_flag quiet;
    std::call_once(quiet, [] { av_log_set_level(AV_LOG_QUIET); });
    // "file:" keeps ffmpeg from reading a drive letter ("C:") as a protocol name. ffmpeg's
    // file protocol takes UTF-8 on every OS (it converts to UTF-16 on Windows).
    const std::string url = "file:" + file::detail::to_utf8(file).text;
    AVFormatContext* raw = avformat_alloc_context();
    if (!raw) return std::unexpected("out of memory");
    // Bounded probing: tags and stream parameters are near the start; never read megabytes.
    raw->probesize = 512 * 1024;
    raw->max_analyze_duration = 2 * AV_TIME_BASE;
    if (const int rc = avformat_open_input(&raw, url.c_str(), nullptr, nullptr); rc < 0) {
        return std::unexpected(error_text(rc));  // avformat_open_input frees the context on failure
    }
    Media media(raw);
    if (probe) {
        bool complete = raw->nb_streams > 0;
        for (unsigned i = 0; i < raw->nb_streams && complete; ++i) {
            const AVStream* s = raw->streams[i];
            if (s->disposition & AV_DISPOSITION_ATTACHED_PIC) continue;
            const AVCodecParameters* p = s->codecpar;
            if (p->codec_type == AVMEDIA_TYPE_AUDIO && (p->sample_rate <= 0 || p->ch_layout.nb_channels <= 0)) complete = false;
            if (p->codec_type == AVMEDIA_TYPE_VIDEO && (p->width <= 0 || p->height <= 0)) complete = false;
        }
        const AVStream* main = media.first_stream(AVMEDIA_TYPE_AUDIO);
        if (!main) main = media.first_stream(AVMEDIA_TYPE_VIDEO);
        if (!complete || media.duration_ms(main) == 0) {
            if (const int rc = avformat_find_stream_info(raw, nullptr); rc < 0) return std::unexpected(error_text(rc));
        }
    }
    return media;
}

AVStream* Media::first_stream(AVMediaType type) const {
    for (unsigned i = 0; i < context_->nb_streams; ++i) {
        AVStream* s = context_->streams[i];
        if (s->codecpar->codec_type == type && !(s->disposition & AV_DISPOSITION_ATTACHED_PIC)) return s;
    }
    return nullptr;
}

AVStream* Media::attached_picture() const {
    for (unsigned i = 0; i < context_->nb_streams; ++i) {
        AVStream* s = context_->streams[i];
        if ((s->disposition & AV_DISPOSITION_ATTACHED_PIC) && s->attached_pic.size > 0) return s;
    }
    return nullptr;
}

std::int64_t Media::duration_ms(const AVStream* stream) const {
    if (stream && stream->duration > 0 && stream->duration != AV_NOPTS_VALUE) {
        return av_rescale_q(stream->duration, stream->time_base, AVRational{1, 1000});
    }
    if (context_->duration > 0 && context_->duration != AV_NOPTS_VALUE) {
        return av_rescale(context_->duration, 1000, AV_TIME_BASE);
    }
    return 0;
}

std::string tag(const AVDictionary* tags, std::initializer_list<const char*> keys) {
    for (const char* key : keys) {
        // av_dict_get ignores case unless asked otherwise.
        if (const AVDictionaryEntry* e = av_dict_get(tags, key, nullptr, 0)) {
            std::string value = core::trim(e->value ? e->value : "");
            if (!value.empty()) return value;
        }
    }
    return {};
}

std::string any_tag(const Media& media, std::initializer_list<const char*> keys) {
    std::string value = tag(media.context()->metadata, keys);
    if (!value.empty()) return value;
    for (AVMediaType type : {AVMEDIA_TYPE_AUDIO, AVMEDIA_TYPE_VIDEO}) {
        if (const AVStream* s = media.first_stream(type)) {
            value = tag(s->metadata, keys);
            if (!value.empty()) return value;
        }
    }
    return {};
}

std::pair<int, int> number_pair(const std::string& text) {
    const auto parse = [](std::string_view part) {
        int value = 0;
        const std::string trimmed = core::trim(part);
        const auto [ptr, ec] = std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), value);
        return (ec == std::errc{} && ptr != trimmed.data() && value > 0 && value < 100000) ? value : 0;
    };
    const auto slash = text.find('/');
    if (slash == std::string::npos) return {parse(text), 0};
    return {parse(std::string_view(text).substr(0, slash)), parse(std::string_view(text).substr(slash + 1))};
}

int year_of(const std::string& date) {
    const std::string d = core::trim(date);
    if (d.size() < 4) return 0;
    int year = 0;
    const auto [ptr, ec] = std::from_chars(d.data(), d.data() + 4, year);
    if (ec != std::errc{} || ptr != d.data() + 4) return 0;
    return (year >= 1000 && year <= 2999) ? year : 0;
}

}
