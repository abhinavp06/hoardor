// video::read / video::embedded_poster (features/media_listing.md §4.3).

#include <hoardor/video/video.hpp>

#include "core/text.hpp"
#include "file/text.hpp"
#include "media/ffmpeg.hpp"
#include "video/sources.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace hoardor::video {

namespace fs = std::filesystem;
using detail::Described;

namespace {

std::string lower_ascii(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return text;
}

std::string extension_of(const fs::path& p) { return lower_ascii(file::detail::to_utf8(p.extension()).text); }
std::string stem_of(const fs::path& p) { return lower_ascii(file::detail::to_utf8(p.stem()).text); }

bool is_image(const fs::path& p) {
    const std::string e = extension_of(p);
    return e == ".jpg" || e == ".jpeg" || e == ".png" || e == ".webp" || e == ".bmp" || e == ".gif";
}

std::string hdr_of(const AVCodecParameters* p) {
    if (av_packet_side_data_get(p->coded_side_data, p->nb_coded_side_data, AV_PKT_DATA_DOVI_CONF)) return "Dolby Vision";
    if (p->color_trc == AVCOL_TRC_SMPTE2084) return "HDR10";
    if (p->color_trc == AVCOL_TRC_ARIB_STD_B67) return "HLG";
    return {};
}

const AVStream* cover_attachment(const AVFormatContext* context) {
    for (unsigned i = 0; i < context->nb_streams; ++i) {
        const AVStream* s = context->streams[i];
        if (s->codecpar->codec_type != AVMEDIA_TYPE_ATTACHMENT || s->codecpar->extradata_size <= 0) continue;
        const std::string name = lower_ascii(media::tag(s->metadata, {"filename"}));
        const std::string mime = lower_ascii(media::tag(s->metadata, {"mimetype"}));
        if (name.starts_with("cover") && (mime.starts_with("image/") || mime.empty())) return s;
    }
    return nullptr;
}

// The poster among the companions, by name, nearest first (see features/media_listing.md §4.3).
int pick_poster(const fs::path& file, std::span<const fs::path> companions, Type type) {
    const fs::path folder = file.parent_path();
    const std::string stem = stem_of(file);
    const auto find = [&](const std::vector<std::string>& names, bool same_folder) {
        for (const std::string& name : names) {
            for (std::size_t i = 0; i < companions.size(); ++i) {
                const fs::path& c = companions[i];
                if (!is_image(c) || (c.parent_path() == folder) != same_folder) continue;
                if (stem_of(c) == name) return static_cast<int>(i);
            }
        }
        return -1;
    };
    if (type == Type::Episode) {
        // The show's poster (the grid shows shows), then the season's.
        if (int i = find({"poster", "folder", "cover", "show"}, false); i >= 0) return i;
        return find({"poster", "folder", "cover", "season"}, true);
    }
    return find({stem + "-poster", "poster", "folder", "cover", "movie"}, true);
}

std::vector<std::string> split_tag(const std::string& value) { return core::split_values(value, std::string_view(";/\0", 3)); }

}

Result<VideoInfo> read(const fs::path& file, std::span<const fs::path> companions) {
    auto media = media::Media::open(file);
    if (!media) return std::unexpected(Error{media.error()});
    const AVStream* video = media->first_stream(AVMEDIA_TYPE_VIDEO);
    if (!video) return std::unexpected(Error{"no video stream"});
    const AVCodecParameters* p = video->codecpar;
    if (p->width <= 0 || p->height <= 0) return std::unexpected(Error{"not a playable video file"});

    VideoInfo info;
    info.width = p->width;
    info.height = p->height;
    info.video_codec = avcodec_get_name(p->codec_id);
    info.hdr = hdr_of(p);
    if (video->avg_frame_rate.num > 0 && video->avg_frame_rate.den > 0) {
        info.frame_rate_milli = static_cast<int>(av_rescale(video->avg_frame_rate.num, 1000, video->avg_frame_rate.den));
    }
    info.duration_ms = media->duration_ms(video);
    const AVFormatContext* context = media->context();
    for (unsigned i = 0; i < context->nb_streams; ++i) {
        const AVStream* s = context->streams[i];
        const AVMediaType type = s->codecpar->codec_type;
        if (type != AVMEDIA_TYPE_AUDIO && type != AVMEDIA_TYPE_SUBTITLE) continue;
        Stream stream{media::tag(s->metadata, {"language"}), avcodec_get_name(s->codecpar->codec_id),
                      type == AVMEDIA_TYPE_AUDIO ? s->codecpar->ch_layout.nb_channels : 0, media::tag(s->metadata, {"title"})};
        (type == AVMEDIA_TYPE_AUDIO ? info.audio : info.subtitles).push_back(std::move(stream));
    }
    info.has_embedded_poster = media->attached_picture() != nullptr || cover_attachment(context) != nullptr;

    // Descriptions: the .nfo next to the file, then the show's tvshow.nfo up the tree.
    std::optional<Described> nfo, show_nfo;
    const fs::path folder = file.parent_path();
    const std::string stem = stem_of(file);
    for (const char* wanted : {"same-stem", "movie"}) {
        for (std::size_t i = 0; i < companions.size() && !nfo; ++i) {
            const fs::path& c = companions[i];
            if (extension_of(c) != ".nfo" || c.parent_path() != folder) continue;
            const std::string s = stem_of(c);
            if ((std::string(wanted) == "same-stem" && s == stem) || (std::string(wanted) == "movie" && s == "movie")) {
                if (auto d = detail::read_nfo(c); d && d->kind != Described::Kind::Show) {
                    nfo = std::move(d);
                    info.info_index = static_cast<int>(i);
                }
            }
        }
    }
    for (const fs::path& c : companions) {
        if (extension_of(c) == ".nfo" && stem_of(c) == "tvshow") {
            if (auto d = detail::read_nfo(c); d && d->kind == Described::Kind::Show) {
                show_nfo = std::move(d);
                break;
            }
        }
    }
    const Described name = detail::from_name(file);
    using media::any_tag;
    const std::string tag_title = any_tag(*media, {"title"});
    const std::string tag_show = any_tag(*media, {"show"});
    const std::string tag_season = any_tag(*media, {"season_number"});
    const std::string tag_episode = any_tag(*media, {"episode_sort", "episode_id"});
    const std::string tag_date = any_tag(*media, {"date_released", "date", "year", "creation_time_original"});

    const bool episode = (nfo && nfo->kind == Described::Kind::Episode) || !tag_show.empty() || !tag_season.empty() ||
                         name.kind == Described::Kind::Episode;
    info.type = episode ? Type::Episode : Type::Movie;

    const auto first = [](std::initializer_list<std::string> values) {
        for (const auto& v : values) {
            if (!v.empty()) return v;
        }
        return std::string();
    };
    const Described none;
    const Described& n = nfo ? *nfo : none;
    const Described& show = show_nfo ? *show_nfo : none;

    info.title = first({n.title, tag_title});
    if (info.title.empty()) {
        info.title = name.title;
        info.from_name = true;
        // An episode with no title at all: "Episode 3".
        if (info.title.empty() && episode && name.episode > 0) info.title = "Episode " + std::to_string(name.episode);
        if (info.title.empty()) info.title = file::detail::to_utf8(file.stem()).text;
    }
    info.source = nfo ? Source::Nfo : (!tag_title.empty() ? Source::Tags : Source::Name);
    if (episode) {
        info.show = first({n.show, show.title, tag_show, name.show});
        info.season = n.season >= 0 ? n.season : (!tag_season.empty() ? media::number_pair(tag_season).first : name.season);
        info.episode = n.episode > 0 ? n.episode : (!tag_episode.empty() ? media::number_pair(tag_episode).first : name.episode);
    }
    info.date = first({n.date, tag_date});
    info.year = n.year > 0 ? n.year : media::year_of(tag_date);
    if (info.year == 0) info.year = episode ? media::year_of(info.date) : name.year;
    if (info.year == 0 && episode) info.year = show.year;
    info.plot = first({n.plot, any_tag(*media, {"description", "synopsis", "comment"})});
    info.genres = !n.genres.empty() ? n.genres : (!show.genres.empty() ? show.genres : split_tag(any_tag(*media, {"genre"})));
    info.directors = !n.directors.empty() ? n.directors : split_tag(any_tag(*media, {"director", "directed_by"}));
    info.writers = n.writers;
    if (info.duration_ms <= 0 && n.runtime_minutes > 0) info.duration_ms = std::int64_t{n.runtime_minutes} * 60'000;
    info.poster_index = pick_poster(file, companions, info.type);
    return info;
}

Result<std::vector<std::byte>> embedded_poster(const fs::path& file) {
    auto media = media::Media::open(file, false);
    if (!media) return std::unexpected(Error{media.error()});
    if (const AVStream* picture = media->attached_picture()) {
        const auto* data = reinterpret_cast<const std::byte*>(picture->attached_pic.data);
        return std::vector<std::byte>(data, data + picture->attached_pic.size);
    }
    if (const AVStream* cover = cover_attachment(media->context())) {
        const auto* data = reinterpret_cast<const std::byte*>(cover->codecpar->extradata);
        return std::vector<std::byte>(data, data + cover->codecpar->extradata_size);
    }
    return std::vector<std::byte>{};
}

}
