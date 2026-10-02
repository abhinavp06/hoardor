#pragma once

// The video engine: what a movie or an episode is (streams, plus descriptions from .nfo
// files, tags, or names) and the generic queries over them (features/media_listing.md
// §4.3, §5). The same query shape as the audio engine.

#include <hoardor/core/page.hpp>
#include <hoardor/db/database.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace hoardor::video {

using EntryId = std::int64_t;
using CategoryId = std::int64_t;
using RootId = std::int64_t;

struct Error {
    std::string message;
};

template <class T>
using Result = std::expected<T, Error>;

// Stored as numbers: never renumber.
enum class Type : std::uint8_t { Movie = 1, Episode = 2 };

// Where the descriptive fields came from.
enum class Source : std::uint8_t { Name = 1, Tags = 2, Nfo = 3 };

struct Stream {
    std::string language;  // ISO 639 as tagged ("eng", "jpn"), or ""
    std::string codec;     // "aac", "eac3", "truehd", "subrip", "hdmv_pgs_subtitle", …
    int channels = 0;      // audio only
    std::string title;     // "Commentary", "SDH", …
};

// ---------------------------------------------------------------- Reading

struct VideoInfo {
    Type type = Type::Movie;
    std::string title;          // the movie's, or the episode's
    std::string show;           // episodes
    int season = -1, episode = 0;  // episodes; season 0 = specials, -1 = unknown
    int year = 0;
    std::string date;           // premiered / aired
    std::string plot;
    std::vector<std::string> genres, directors, writers;
    std::int64_t duration_ms = 0;
    int width = 0, height = 0;
    std::string hdr;            // "", "HDR10", "HLG", "Dolby Vision"
    std::string video_codec;    // "hevc", "h264", "av1", …
    int frame_rate_milli = 0;   // 23976 = 23.976 fps
    std::vector<Stream> audio, subtitles;
    Source source = Source::Name;
    bool from_name = false;     // the title came from the file name (no .nfo, no title tag)
    int info_index = -1;        // which companion .nfo was used (index into `companions`), or -1
    int poster_index = -1;      // which companion image is the poster, or -1
    bool has_embedded_poster = false;
};

// Opens the file (touches the drive). `companions` are the image and .nfo files next to it
// and up to two folders up (file::Library::companions), nearest first.
Result<VideoInfo> read(const std::filesystem::path& file, std::span<const std::filesystem::path> companions = {});

// A poster stored inside the file (a Matroska "cover" attachment, or an MP4 cover), or empty.
Result<std::vector<std::byte>> embedded_poster(const std::filesystem::path& file);

// ---------------------------------------------------------------- Queries

enum class Field : std::uint8_t {
    Type, Title, Year, Genre, Director, Show, Season, Episode,  // descriptive (Genre, Director: many per item)
    Duration, Height, Hdr, VideoCodec,                          // technical
    Added, Category, Root, Entry,                               // from the file engine
};

using Value = std::variant<std::int64_t, std::string>;

struct Condition {
    Field field;
    Value value;  // Type: 1 movie, 2 episode
};

struct Filter {
    std::vector<Condition> all;
};

struct Order {
    Field field;  // Genre and Director can't be ordered
    bool descending = false;
};

struct Item {
    EntryId entry_id = 0;
    Type type = Type::Movie;
    std::string title, show;
    int season = -1, episode = 0, year = 0;
    std::string date, plot;
    std::vector<std::string> genres, directors;
    std::int64_t duration_ms = 0;
    int width = 0, height = 0;
    std::string hdr, video_codec;
    int frame_rate_milli = 0;
    std::vector<Stream> audio, subtitles;
    Source source = Source::Name;
    bool from_name = false;
    EntryId poster_entry = 0;        // a companion image file, or 0
    bool has_embedded_poster = false;
    // From the file engine:
    RootId root_id = 0;
    bool root_online = false;
    std::int64_t added_ns = 0;
    std::uint64_t size = 0;
};

struct Group {
    std::vector<std::string> values;  // the group-by fields' display values
    std::uint64_t items = 0;
    std::int64_t duration_ms = 0;
    std::int64_t added_first_ns = 0, added_last_ns = 0;
    int year_min = 0, year_max = 0;
    int max_height = 0;               // the best resolution among the group's items
    bool any_hdr = false;
    EntryId poster_entry = 0;         // a companion image, if any item has one
    EntryId embedded_poster_entry = 0;  // else a video with a poster inside it
    bool any_online = false;
};

enum class GroupOrder : std::uint8_t { Values, AddedLast, Year, Items };

struct PendingEntry {
    EntryId entry_id = 0;
    RootId root_id = 0;
    std::string relative_path;
    std::int64_t size = 0, mtime_ns = 0;
};

class Library {
public:
    // Runs the video engine's migrations (after file::Library::open).
    static Result<Library> open(db::Database& database);

    Result<std::vector<PendingEntry>> pending(std::optional<CategoryId> category, EntryId after = 0, std::size_t limit = 200);
    Result<std::uint64_t> pending_count(std::optional<CategoryId> category);
    // poster_entry: the file entry of the companion image read() chose (0: none).
    Result<void> store(EntryId entry, std::int64_t source_size, std::int64_t source_mtime_ns, const VideoInfo& info,
                       EntryId poster_entry = 0);
    Result<void> store_error(EntryId entry, std::int64_t source_size, std::int64_t source_mtime_ns, std::string_view message);
    Result<std::uint64_t> remove_unused_names();

    Result<core::Page<Item>> items(const Filter& filter, std::span<const Order> order = {},
                                   const std::optional<core::Cursor>& after = std::nullopt, std::size_t limit = 200);
    Result<core::Page<Group>> groups(std::span<const Field> by, const Filter& filter, GroupOrder order = GroupOrder::Values,
                                     bool descending = false, const std::optional<core::Cursor>& after = std::nullopt,
                                     std::size_t limit = 200);
    Result<std::uint64_t> count(const Filter& filter);
    Result<std::uint64_t> group_count(std::span<const Field> by, const Filter& filter);
    Result<Item> item(EntryId entry);

private:
    explicit Library(db::Database& database) : db_(&database) {}
    db::Database* db_;
};

}
