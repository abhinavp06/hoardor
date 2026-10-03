#pragma once

// The audio engine: what's in an audio file (tags and stream information) and the
// generic queries over every track (features/media_listing.md §4.2, §5).
// Engines never call each other: master hands this engine paths to read
// (Library::pending) and stores the results.

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

namespace hoardor::audio {

using EntryId = std::int64_t;     // a file::Entry id
using CategoryId = std::int64_t;  // a file::Category id
using RootId = std::int64_t;      // a file::Root id

struct Error {
    std::string message;
};

template <class T>
using Result = std::expected<T, Error>;

// ---------------------------------------------------------------- Reading

// Everything read from one file. Text is exactly as tagged.
struct TrackInfo {
    std::string title, album, album_artist;
    std::vector<std::string> artists, genres;
    std::string title_sort, album_sort, album_artist_sort, artist_sort;  // sort tags, when present
    int track = 0, track_total = 0, disc = 0, disc_total = 0;
    std::string date;  // as tagged: "1997", "1997-05-21"
    int year = 0;      // from the date
    std::int64_t duration_ms = 0;
    int bitrate_kbps = 0, sample_rate = 0, bit_depth = 0, channels = 0;  // bit_depth 0: not meaningful (lossy)
    std::string codec;  // ffmpeg's codec name: "flac", "mp3", "aac", "alac", "opus", "vorbis", "pcm_s24le", …
    bool lossless = false;
    bool has_embedded_cover = false;
    bool title_from_name = false;  // no title tag: the file name stands in
    bool album_from_name = false;  // no album tag: the folder name stands in
};

// Opens the file and reads its header (touches the drive). Missing tags fall back so
// every track can be listed: title -> file name, album -> folder name, album artist ->
// first artist -> "Unknown artist".
Result<TrackInfo> read(const std::filesystem::path& file);

// The embedded cover picture's bytes (JPEG or PNG), or empty if there is none.
// Touches the drive: for thumbnails, never for browsing.
Result<std::vector<std::byte>> embedded_cover(const std::filesystem::path& file);

// ---------------------------------------------------------------- Queries

enum class Field : std::uint8_t {
    Title, Artist, AlbumArtist, Album, Genre, Year, Disc, Track,  // descriptive (Artist, Genre: many per track)
    Duration, Bitrate, SampleRate, BitDepth, Codec, Lossless,     // technical
    Added, Category, Root, Entry,                                 // from the file engine
    Search,  // filter only: words matched (as prefixes, all required) in title, album, album artist, artists, genres
};

using Value = std::variant<std::int64_t, std::string>;

// Equality. Text matches ignoring ASCII case and a leading article ("The Beatles" = "beatles");
// Artist and Genre match any of a track's values.
struct Condition {
    Field field;
    Value value;
};

struct Filter {
    std::vector<Condition> all;  // ANDed; empty: every track
};

struct Order {
    Field field;  // Artist orders by the first artist; Genre can't be ordered
    bool descending = false;
};

struct Track {
    EntryId entry_id = 0;
    std::string title, album, album_artist;
    std::vector<std::string> artists, genres;
    int track = 0, track_total = 0, disc = 0, disc_total = 0, year = 0;
    std::string date;
    std::int64_t duration_ms = 0;
    int bitrate_kbps = 0, sample_rate = 0, bit_depth = 0, channels = 0;
    std::string codec;
    bool lossless = false, has_embedded_cover = false, title_from_name = false, album_from_name = false;
    // From the file engine:
    RootId root_id = 0;
    bool root_online = false;
    std::int64_t added_ns = 0;
    std::uint64_t size = 0;
};

// One group of tracks sharing the group-by fields' values (e.g. an album).
struct Group {
    std::vector<std::string> values;  // display values of the group-by fields, in order (numbers as text)
    std::uint64_t tracks = 0;
    std::int64_t duration_ms = 0;
    std::int64_t added_first_ns = 0, added_last_ns = 0;
    int year_min = 0, year_max = 0;   // 0: no year tagged
    EntryId cover_entry = 0;          // a track to take art from: one with embedded art if any, else the first
    bool any_online = false;
};

enum class GroupOrder : std::uint8_t {
    Values,     // the group-by fields' sort keys, in order
    AddedLast,  // when the group last got a track
    Year,       // the group's latest year
    Tracks,     // how many tracks
};

// A file that needs (re)reading: never read, or changed since.
struct PendingEntry {
    EntryId entry_id = 0;
    RootId root_id = 0;
    std::string relative_path;
    std::int64_t size = 0, mtime_ns = 0;
};

// The audio engine's repository on one connection (one thread at a time).
class Library {
public:
    // Runs the audio engine's migrations. The file engine's tables must exist
    // (file::Library::open first). `database` must outlive the Library.
    static Result<Library> open(db::Database& database);

    // ---- Reading support (driven by master) ----

    // Settled audio entries with no metadata or stale metadata, by id, in online roots
    // (of one category, or all). Reads SQLite only.
    // `root`: only that folder's entries (master reads each drive on its own worker).
    Result<std::vector<PendingEntry>> pending(std::optional<CategoryId> category, EntryId after = 0, std::size_t limit = 200,
                                              std::optional<RootId> root = std::nullopt);
    Result<std::uint64_t> pending_count(std::optional<CategoryId> category, std::optional<RootId> root = std::nullopt);
    // Stores what read() returned. Runs inside the caller's transaction, if any.
    Result<void> store(EntryId entry, std::int64_t source_size, std::int64_t source_mtime_ns, const TrackInfo& info);
    // Records that the file couldn't be read, so it isn't retried until it changes.
    Result<void> store_error(EntryId entry, std::int64_t source_size, std::int64_t source_mtime_ns, std::string_view message);
    // Deletes artists and genres no track uses any more. Returns how many.
    Result<std::uint64_t> remove_unused_names();

    // ---- Queries (SQLite only; unreadable files are left out) ----

    Result<core::Page<Track>> tracks(const Filter& filter, std::span<const Order> order = {},
                                     const std::optional<core::Cursor>& after = std::nullopt, std::size_t limit = 200);
    Result<core::Page<Group>> groups(std::span<const Field> by, const Filter& filter, GroupOrder order = GroupOrder::Values,
                                     bool descending = false, const std::optional<core::Cursor>& after = std::nullopt,
                                     std::size_t limit = 200);
    Result<std::uint64_t> count(const Filter& filter);
    Result<std::uint64_t> group_count(std::span<const Field> by, const Filter& filter);
    // One track by entry id (NotFound as an error message).
    Result<Track> track(EntryId entry);

private:
    explicit Library(db::Database& database) : db_(&database) {}
    db::Database* db_;
};

}
