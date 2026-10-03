// audio::read / audio::embedded_cover: one file's tags and stream info through ffmpeg
// (features/media_listing.md §4.2).

#include <hoardor/audio/audio.hpp>

#include "core/text.hpp"
#include "file/text.hpp"
#include "media/ffmpeg.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/samplefmt.h>
}

namespace hoardor::audio {

namespace {

bool is_lossless(AVCodecID id) {
    if (id >= AV_CODEC_ID_PCM_S16LE && id < AV_CODEC_ID_ADPCM_IMA_QT) return true;  // every PCM variant
    switch (id) {
        case AV_CODEC_ID_FLAC:
        case AV_CODEC_ID_ALAC:
        case AV_CODEC_ID_APE:
        case AV_CODEC_ID_WAVPACK:
        case AV_CODEC_ID_TTA:
        case AV_CODEC_ID_MLP:
        case AV_CODEC_ID_TRUEHD:
        case AV_CODEC_ID_WMALOSSLESS:
        case AV_CODEC_ID_DSD_LSBF:
        case AV_CODEC_ID_DSD_MSBF:
        case AV_CODEC_ID_DSD_LSBF_PLANAR:
        case AV_CODEC_ID_DSD_MSBF_PLANAR:
            return true;
        default:
            return false;
    }
}

// Multi-valued tags: ffmpeg joins repeated Vorbis comments with ";" and keeps ID3v2.4's NUL separators.
const std::string_view value_separators{";\0", 2};
const std::string_view genre_separators{";/\0", 3};

}

Result<TrackInfo> read(const std::filesystem::path& file) {
    auto media = media::Media::open(file);
    if (!media) return std::unexpected(Error{media.error()});
    const AVStream* stream = media->first_stream(AVMEDIA_TYPE_AUDIO);
    if (!stream) return std::unexpected(Error{"no audio stream"});
    const AVCodecParameters* p = stream->codecpar;
    using media::any_tag;

    TrackInfo t;
    t.title = any_tag(*media, {"title"});
    t.artists = core::split_values(any_tag(*media, {"artist"}), value_separators);
    t.album_artist = any_tag(*media, {"album_artist", "albumartist", "album artist", "TPE2"});
    t.album = any_tag(*media, {"album"});
    t.genres = core::split_values(any_tag(*media, {"genre"}), genre_separators);
    t.title_sort = any_tag(*media, {"titlesort", "title-sort", "sort_name", "TSOT"});
    t.album_sort = any_tag(*media, {"albumsort", "album-sort", "sort_album", "TSOA"});
    t.artist_sort = any_tag(*media, {"artistsort", "artist-sort", "sort_artist", "TSOP"});
    t.album_artist_sort = any_tag(*media, {"albumartistsort", "album_artist-sort", "sort_album_artist", "TSO2"});

    std::tie(t.track, t.track_total) = media::number_pair(any_tag(*media, {"track", "tracknumber"}));
    if (t.track_total == 0) t.track_total = media::number_pair(any_tag(*media, {"tracktotal", "totaltracks"})).first;
    std::tie(t.disc, t.disc_total) = media::number_pair(any_tag(*media, {"disc", "discnumber"}));
    if (t.disc_total == 0) t.disc_total = media::number_pair(any_tag(*media, {"disctotal", "totaldiscs"})).first;
    t.date = any_tag(*media, {"date", "year", "originaldate", "original_date", "TDRC", "TYER"});
    t.year = media::year_of(t.date);

    t.codec = avcodec_get_name(p->codec_id);
    t.lossless = is_lossless(p->codec_id);
    t.sample_rate = p->sample_rate;
    t.channels = p->ch_layout.nb_channels;
    if (p->bits_per_raw_sample > 0) t.bit_depth = p->bits_per_raw_sample;
    else if (t.lossless && p->bits_per_coded_sample > 0) t.bit_depth = p->bits_per_coded_sample;
    else if (t.lossless) t.bit_depth = av_get_bytes_per_sample(static_cast<AVSampleFormat>(p->format)) * 8;
    t.duration_ms = media->duration_ms(stream);
    // ffmpeg guesses a format even for random bytes (often "flac" or "mp3"), with no sample rate
    // or channels: then there is nothing to play. An unknown length is fine: a FLAC whose header
    // says "0 samples" (some encoders, and ffmpeg itself when it adds a cover) still plays, and
    // its length stays 0 until something measures it (2026-10-03; such files were rejected).
    if (t.sample_rate <= 0 || t.channels <= 0) return std::unexpected(Error{"not a playable audio file"});

    std::int64_t bits_per_second = p->bit_rate > 0 ? p->bit_rate : media->context()->bit_rate;
    if (bits_per_second <= 0 && t.duration_ms > 0) {
        std::error_code ec;
        const auto size = std::filesystem::file_size(file, ec);
        if (!ec) bits_per_second = static_cast<std::int64_t>(size) * 8000 / t.duration_ms;
    }
    t.bitrate_kbps = static_cast<int>((bits_per_second + 500) / 1000);
    t.has_embedded_cover = media->attached_picture() != nullptr;

    // Fallbacks, so every track can be listed and grouped.
    if (t.title.empty()) {
        t.title = file::detail::to_utf8(file.stem()).text;
        t.title_from_name = true;
    }
    if (t.album.empty()) {
        t.album = file::detail::to_utf8(file.parent_path().filename()).text;
        t.album_from_name = true;
    }
    if (t.album_artist.empty()) t.album_artist = t.artists.empty() ? std::string("Unknown artist") : t.artists.front();
    return t;
}

Result<std::vector<std::byte>> embedded_cover(const std::filesystem::path& file) {
    auto media = media::Media::open(file, false);
    if (!media) return std::unexpected(Error{media.error()});
    const AVStream* picture = media->attached_picture();
    if (!picture) return std::vector<std::byte>{};
    const auto* data = reinterpret_cast<const std::byte*>(picture->attached_pic.data);
    return std::vector<std::byte>(data, data + picture->attached_pic.size);
}

}
