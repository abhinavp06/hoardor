// audio::Library: the audio engine's tables and generic queries (features/media_listing.md §5, §6).

#include <hoardor/audio/audio.hpp>

#include "core/text.hpp"
#include "media/query.hpp"

#include <array>

namespace hoardor::audio {

namespace {

Error database_error(const db::Error& e) { return Error{e.message}; }

constexpr std::string_view schema_v1 = R"sql(
CREATE TABLE audio_tracks (
    entry_id INTEGER PRIMARY KEY REFERENCES file_entries(id) ON DELETE CASCADE,
    source_size INTEGER NOT NULL,
    source_mtime_ns INTEGER NOT NULL,
    read_error TEXT NOT NULL DEFAULT '',
    title TEXT NOT NULL DEFAULT '', title_key TEXT NOT NULL DEFAULT '', title_from_name INTEGER NOT NULL DEFAULT 0,
    album TEXT NOT NULL DEFAULT '', album_key TEXT NOT NULL DEFAULT '', album_from_name INTEGER NOT NULL DEFAULT 0,
    album_artist TEXT NOT NULL DEFAULT '', album_artist_key TEXT NOT NULL DEFAULT '',
    artist TEXT NOT NULL DEFAULT '', artist_key TEXT NOT NULL DEFAULT '',
    genre TEXT NOT NULL DEFAULT '',
    track INTEGER NOT NULL DEFAULT 0, track_total INTEGER NOT NULL DEFAULT 0,
    disc INTEGER NOT NULL DEFAULT 0, disc_total INTEGER NOT NULL DEFAULT 0,
    date TEXT NOT NULL DEFAULT '', year INTEGER NOT NULL DEFAULT 0,
    duration_ms INTEGER NOT NULL DEFAULT 0, bitrate_kbps INTEGER NOT NULL DEFAULT 0,
    sample_rate INTEGER NOT NULL DEFAULT 0, bit_depth INTEGER NOT NULL DEFAULT 0, channels INTEGER NOT NULL DEFAULT 0,
    codec TEXT NOT NULL DEFAULT '', lossless INTEGER NOT NULL DEFAULT 0, has_embedded_cover INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX audio_tracks_album ON audio_tracks (album_artist_key, album_key, disc, track);
CREATE INDEX audio_tracks_album_title ON audio_tracks (album_key, album_artist_key);
CREATE INDEX audio_tracks_year ON audio_tracks (year, album_artist_key, album_key);
CREATE INDEX audio_tracks_title ON audio_tracks (title_key);
CREATE TABLE audio_names (
    id INTEGER PRIMARY KEY,
    kind INTEGER NOT NULL,   -- 1 artist, 2 genre
    name TEXT NOT NULL,      -- as first seen
    key TEXT NOT NULL,       -- core::sort_key(name): the identity
    UNIQUE (kind, key)
);
CREATE TABLE audio_track_names (
    entry_id INTEGER NOT NULL REFERENCES audio_tracks(entry_id) ON DELETE CASCADE,
    name_id INTEGER NOT NULL REFERENCES audio_names(id),
    position INTEGER NOT NULL,
    PRIMARY KEY (entry_id, name_id)
) WITHOUT ROWID;
CREATE INDEX audio_track_names_name ON audio_track_names (name_id, entry_id);
)sql";

// Full-text search (features/media_listing.md §8c): one FTS5 row per readable track, rowid =
// entry_id, kept current by triggers (foreign-key cascades fire them too). Diacritics are
// folded, so "bjork" finds "Björk".
constexpr std::string_view schema_v2 = R"sql(
CREATE VIRTUAL TABLE audio_search USING fts5(title, album, album_artist, artists, genres,
                                             tokenize = 'unicode61 remove_diacritics 2');
INSERT INTO audio_search (rowid, title, album, album_artist, artists, genres)
    SELECT entry_id, title, album, album_artist, artist, genre FROM audio_tracks WHERE read_error = '';
CREATE TRIGGER audio_search_insert AFTER INSERT ON audio_tracks WHEN new.read_error = '' BEGIN
    INSERT INTO audio_search (rowid, title, album, album_artist, artists, genres)
        VALUES (new.entry_id, new.title, new.album, new.album_artist, new.artist, new.genre);
END;
CREATE TRIGGER audio_search_update AFTER UPDATE ON audio_tracks BEGIN
    DELETE FROM audio_search WHERE rowid = old.entry_id;
    INSERT INTO audio_search (rowid, title, album, album_artist, artists, genres)
        SELECT new.entry_id, new.title, new.album, new.album_artist, new.artist, new.genre WHERE new.read_error = '';
END;
CREATE TRIGGER audio_search_delete AFTER DELETE ON audio_tracks BEGIN
    DELETE FROM audio_search WHERE rowid = old.entry_id;
END;
)sql";

// Read once more what was rejected as "not a playable audio file": a FLAC with an unknown
// length (STREAMINFO says 0 samples) was rejected until 2026-10-03, and an unreadable row is
// otherwise only retried when its file changes. A size that can't match makes it pending.
constexpr std::string_view schema_v3 = R"sql(
UPDATE audio_tracks SET source_size = -1 WHERE read_error = 'not a playable audio file';
)sql";

constexpr std::array<db::Migration, 3> migrations{{{1, schema_v1}, {2, schema_v2}, {3, schema_v3}}};

constexpr int artist_kind = 1;
constexpr int genre_kind = 2;

// Lists stored in one column, one value per line (a tag value never contains a newline).
std::string join(const std::vector<std::string>& values) {
    std::string out;
    for (const auto& v : values) out += (out.empty() ? "" : "\n") + v;
    return out;
}

std::vector<std::string> split(const std::string& text) {
    std::vector<std::string> out;
    if (text.empty()) return out;
    std::size_t start = 0;
    while (true) {
        const auto end = text.find('\n', start);
        out.push_back(text.substr(start, end - start));
        if (end == std::string::npos) return out;
        start = end + 1;
    }
}

const media::Schema& schema() {
    static const media::Schema s = [] {
        media::Schema m;
        m.table = "audio_tracks t";
        m.joins = " JOIN file_entries e ON e.id = t.entry_id JOIN file_roots r ON r.id = e.root_id";
        m.id = "t.entry_id";
        m.base_where = "t.read_error = ''";
        m.link_table = "audio_track_names";
        m.names_table = "audio_names";
        const auto text = [](std::string value, std::string key) { return media::FieldSql{.value = value, .key = key, .text = true}; };
        const auto number = [](std::string column) { return media::FieldSql{.value = column, .key = column}; };
        m.fields[int(Field::Title)] = text("t.title", "t.title_key");
        m.fields[int(Field::Artist)] = media::FieldSql{.value = "t.artist", .key = "t.artist_key", .text = true, .names_kind = artist_kind};
        m.fields[int(Field::AlbumArtist)] = text("t.album_artist", "t.album_artist_key");
        m.fields[int(Field::Album)] = text("t.album", "t.album_key");
        m.fields[int(Field::Genre)] = media::FieldSql{.value = "t.genre", .key = "", .text = true, .names_kind = genre_kind};
        m.fields[int(Field::Year)] = number("t.year");
        m.fields[int(Field::Disc)] = number("t.disc");
        m.fields[int(Field::Track)] = number("t.track");
        m.fields[int(Field::Duration)] = number("t.duration_ms");
        m.fields[int(Field::Bitrate)] = number("t.bitrate_kbps");
        m.fields[int(Field::SampleRate)] = number("t.sample_rate");
        m.fields[int(Field::BitDepth)] = number("t.bit_depth");
        m.fields[int(Field::Codec)] = media::FieldSql{.value = "t.codec", .key = "t.codec", .text = true, .normalized = false};
        m.fields[int(Field::Lossless)] = number("t.lossless");
        m.fields[int(Field::Added)] = number("e.added_ns");
        m.fields[int(Field::Category)] = media::FieldSql{.value = "r.category_id", .key = "r.category_id", .broad = true};
        m.fields[int(Field::Root)] = media::FieldSql{.value = "e.root_id", .key = "e.root_id", .broad = true};
        m.fields[int(Field::Entry)] = number("t.entry_id");
        m.fields[int(Field::Search)] = media::FieldSql{.value = "", .key = "", .text = true, .search_table = "audio_search"};
        m.group_indexes[{int(Field::AlbumArtist), int(Field::Album)}] = "audio_tracks_album";
        m.group_indexes[{int(Field::AlbumArtist)}] = "audio_tracks_album";
        m.group_indexes[{int(Field::Album), int(Field::AlbumArtist)}] = "audio_tracks_album_title";
        m.group_indexes[{int(Field::Year), int(Field::AlbumArtist), int(Field::Album)}] = "audio_tracks_year";
        return m;
    }();
    return s;
}

constexpr std::string_view track_columns =
    "t.entry_id, t.title, t.album, t.album_artist, t.artist, t.genre, t.track, t.track_total, t.disc, t.disc_total, "
    "t.year, t.date, t.duration_ms, t.bitrate_kbps, t.sample_rate, t.bit_depth, t.channels, t.codec, t.lossless, "
    "t.has_embedded_cover, t.title_from_name, t.album_from_name, e.root_id, r.status, e.added_ns, e.size";
constexpr std::size_t track_column_count = 26;

Track read_track(const db::Statement& st) {
    Track t;
    t.entry_id = st.column_int64(0);
    t.title = st.column_text(1);
    t.album = st.column_text(2);
    t.album_artist = st.column_text(3);
    t.artists = split(st.column_text(4));
    t.genres = split(st.column_text(5));
    t.track = static_cast<int>(st.column_int64(6));
    t.track_total = static_cast<int>(st.column_int64(7));
    t.disc = static_cast<int>(st.column_int64(8));
    t.disc_total = static_cast<int>(st.column_int64(9));
    t.year = static_cast<int>(st.column_int64(10));
    t.date = st.column_text(11);
    t.duration_ms = st.column_int64(12);
    t.bitrate_kbps = static_cast<int>(st.column_int64(13));
    t.sample_rate = static_cast<int>(st.column_int64(14));
    t.bit_depth = static_cast<int>(st.column_int64(15));
    t.channels = static_cast<int>(st.column_int64(16));
    t.codec = st.column_text(17);
    t.lossless = st.column_int64(18) != 0;
    t.has_embedded_cover = st.column_int64(19) != 0;
    t.title_from_name = st.column_int64(20) != 0;
    t.album_from_name = st.column_int64(21) != 0;
    t.root_id = st.column_int64(22);
    t.root_online = st.column_int64(23) == 1;  // file::RootStatus::Online
    t.added_ns = st.column_int64(24);
    t.size = static_cast<std::uint64_t>(st.column_int64(25));
    return t;
}

constexpr std::string_view group_aggregates =
    "COUNT(*), SUM(t.duration_ms), MIN(e.added_ns), MAX(e.added_ns), COALESCE(MIN(NULLIF(t.year, 0)), 0), MAX(t.year), "
    "COALESCE(MIN(CASE WHEN t.has_embedded_cover THEN t.entry_id END), MIN(t.entry_id)), MAX(r.status = 1)";
constexpr std::size_t group_aggregate_count = 8;

std::string_view order_sql(GroupOrder order) {
    switch (order) {
        case GroupOrder::AddedLast: return "MAX(e.added_ns)";
        case GroupOrder::Year: return "MAX(t.year)";
        case GroupOrder::Tracks: return "COUNT(*)";
        case GroupOrder::Values: break;
    }
    return "";
}

std::vector<media::Condition> conditions(const Filter& filter) {
    std::vector<media::Condition> out;
    for (const auto& c : filter.all) out.push_back(media::Condition{int(c.field), c.value});
    return out;
}

Result<std::uint64_t> single_count(db::Database& db, const media::BuildResult& built) {
    if (!built) return std::unexpected(Error{built.error()});
    auto st = db.prepare(built->sql);
    if (!st) return std::unexpected(database_error(st.error()));
    media::bind_all(*st, built->binds);
    auto row = st->step();
    if (!row) return std::unexpected(database_error(row.error()));
    return static_cast<std::uint64_t>(st->column_int64(0));
}

constexpr std::string_view pending_sql =
    "FROM file_entries e JOIN file_roots r ON r.id = e.root_id LEFT JOIN audio_tracks t ON t.entry_id = e.id "
    "WHERE e.kind = 1 AND e.unsettled = 0 AND r.status = 1 "
    "AND (t.entry_id IS NULL OR t.source_size <> e.size OR t.source_mtime_ns <> e.mtime_ns) "
    "AND (? IS NULL OR r.category_id = ?) AND (? IS NULL OR e.root_id = ?)";

}

Result<Library> Library::open(db::Database& database) {
    if (auto r = db::migrate(database, "audio", migrations); !r) return std::unexpected(database_error(r.error()));
    return Library(database);
}

Result<std::vector<PendingEntry>> Library::pending(std::optional<CategoryId> category, EntryId after, std::size_t limit,
                                                   std::optional<RootId> root) {
    auto st = db_->prepare("SELECT e.id, e.root_id, e.relative_path, e.size, e.mtime_ns " + std::string(pending_sql) +
                           " AND e.id > ? ORDER BY e.id LIMIT ?");
    if (!st) return std::unexpected(database_error(st.error()));
    if (category) st->bind(1, *category).bind(2, *category);
    else st->bind_null(1).bind_null(2);
    if (root) st->bind(3, *root).bind(4, *root);
    else st->bind_null(3).bind_null(4);
    st->bind(5, after).bind(6, static_cast<std::int64_t>(limit));
    std::vector<PendingEntry> out;
    while (true) {
        auto row = st->step();
        if (!row) return std::unexpected(database_error(row.error()));
        if (!*row) return out;
        out.push_back(PendingEntry{st->column_int64(0), st->column_int64(1), st->column_text(2), st->column_int64(3),
                                   st->column_int64(4)});
    }
}

Result<std::uint64_t> Library::pending_count(std::optional<CategoryId> category, std::optional<RootId> root) {
    auto st = db_->prepare("SELECT COUNT(*) " + std::string(pending_sql));
    if (!st) return std::unexpected(database_error(st.error()));
    if (category) st->bind(1, *category).bind(2, *category);
    else st->bind_null(1).bind_null(2);
    if (root) st->bind(3, *root).bind(4, *root);
    else st->bind_null(3).bind_null(4);
    auto row = st->step();
    if (!row) return std::unexpected(database_error(row.error()));
    return static_cast<std::uint64_t>(st->column_int64(0));
}

Result<void> Library::store(EntryId entry, std::int64_t source_size, std::int64_t source_mtime_ns, const TrackInfo& info) {
    const auto key = [](const std::string& sort, const std::string& value) {
        return core::sort_key(sort.empty() ? value : sort);
    };
    auto st = db_->prepare(
        "INSERT INTO audio_tracks (entry_id, source_size, source_mtime_ns, read_error, title, title_key, title_from_name, "
        "album, album_key, album_from_name, album_artist, album_artist_key, artist, artist_key, genre, track, track_total, "
        "disc, disc_total, date, year, duration_ms, bitrate_kbps, sample_rate, bit_depth, channels, codec, lossless, "
        "has_embedded_cover) VALUES (?, ?, ?, '', ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT (entry_id) DO UPDATE SET source_size = excluded.source_size, source_mtime_ns = excluded.source_mtime_ns, "
        "read_error = '', title = excluded.title, title_key = excluded.title_key, title_from_name = excluded.title_from_name, "
        "album = excluded.album, album_key = excluded.album_key, album_from_name = excluded.album_from_name, "
        "album_artist = excluded.album_artist, album_artist_key = excluded.album_artist_key, artist = excluded.artist, "
        "artist_key = excluded.artist_key, genre = excluded.genre, track = excluded.track, track_total = excluded.track_total, "
        "disc = excluded.disc, disc_total = excluded.disc_total, date = excluded.date, year = excluded.year, "
        "duration_ms = excluded.duration_ms, bitrate_kbps = excluded.bitrate_kbps, sample_rate = excluded.sample_rate, "
        "bit_depth = excluded.bit_depth, channels = excluded.channels, codec = excluded.codec, lossless = excluded.lossless, "
        "has_embedded_cover = excluded.has_embedded_cover");
    if (!st) return std::unexpected(database_error(st.error()));
    const std::string first_artist = info.artists.empty() ? std::string() : info.artists.front();
    st->bind(1, entry)
        .bind(2, source_size)
        .bind(3, source_mtime_ns)
        .bind(4, std::string_view(info.title))
        .bind(5, std::string_view(key(info.title_sort, info.title)))
        .bind(6, info.title_from_name ? 1 : 0)
        .bind(7, std::string_view(info.album))
        .bind(8, std::string_view(key(info.album_sort, info.album)))
        .bind(9, info.album_from_name ? 1 : 0)
        .bind(10, std::string_view(info.album_artist))
        .bind(11, std::string_view(key(info.album_artist_sort, info.album_artist)))
        .bind(12, std::string_view(join(info.artists)))
        .bind(13, std::string_view(key(info.artist_sort, first_artist)))
        .bind(14, std::string_view(join(info.genres)))
        .bind(15, info.track)
        .bind(16, info.track_total)
        .bind(17, info.disc)
        .bind(18, info.disc_total)
        .bind(19, std::string_view(info.date))
        .bind(20, info.year)
        .bind(21, info.duration_ms)
        .bind(22, info.bitrate_kbps)
        .bind(23, info.sample_rate)
        .bind(24, info.bit_depth)
        .bind(25, info.channels)
        .bind(26, std::string_view(info.codec))
        .bind(27, info.lossless ? 1 : 0)
        .bind(28, info.has_embedded_cover ? 1 : 0);
    if (auto r = st->run(); !r) return std::unexpected(database_error(r.error()));

    auto clear = db_->prepare("DELETE FROM audio_track_names WHERE entry_id = ?");
    auto add_name = db_->prepare("INSERT INTO audio_names (kind, name, key) VALUES (?, ?, ?) ON CONFLICT (kind, key) DO NOTHING");
    auto find_name = db_->prepare("SELECT id FROM audio_names WHERE kind = ? AND key = ?");
    auto link = db_->prepare("INSERT OR IGNORE INTO audio_track_names (entry_id, name_id, position) VALUES (?, ?, ?)");
    for (auto* s : {&clear, &add_name, &find_name, &link}) {
        if (!*s) return std::unexpected(database_error(s->error()));
    }
    clear->bind(1, entry);
    if (auto r = clear->run(); !r) return std::unexpected(database_error(r.error()));
    const auto store_names = [&](int kind, const std::vector<std::string>& names) -> Result<void> {
        for (std::size_t i = 0; i < names.size(); ++i) {
            const std::string k = core::sort_key(names[i]);
            add_name->bind(1, kind).bind(2, std::string_view(names[i])).bind(3, std::string_view(k));
            if (auto r = add_name->run(); !r) return std::unexpected(database_error(r.error()));
            add_name->reset();
            find_name->bind(1, kind).bind(2, std::string_view(k));
            auto row = find_name->step();
            if (!row) return std::unexpected(database_error(row.error()));
            const std::int64_t name_id = find_name->column_int64(0);
            find_name->reset();
            link->bind(1, entry).bind(2, name_id).bind(3, static_cast<std::int64_t>(i));
            if (auto r = link->run(); !r) return std::unexpected(database_error(r.error()));
            link->reset();
        }
        return {};
    };
    if (auto r = store_names(artist_kind, info.artists); !r) return r;
    return store_names(genre_kind, info.genres);
}

Result<void> Library::store_error(EntryId entry, std::int64_t source_size, std::int64_t source_mtime_ns,
                                  std::string_view message) {
    auto del = db_->prepare("DELETE FROM audio_tracks WHERE entry_id = ?");
    if (!del) return std::unexpected(database_error(del.error()));
    del->bind(1, entry);
    if (auto r = del->run(); !r) return std::unexpected(database_error(r.error()));
    auto st = db_->prepare("INSERT INTO audio_tracks (entry_id, source_size, source_mtime_ns, read_error) VALUES (?, ?, ?, ?)");
    if (!st) return std::unexpected(database_error(st.error()));
    st->bind(1, entry).bind(2, source_size).bind(3, source_mtime_ns).bind(4, message.empty() ? std::string_view("unreadable") : message);
    if (auto r = st->run(); !r) return std::unexpected(database_error(r.error()));
    return {};
}

Result<std::uint64_t> Library::remove_unused_names() {
    auto st = db_->prepare("DELETE FROM audio_names WHERE NOT EXISTS (SELECT 1 FROM audio_track_names l WHERE l.name_id = audio_names.id)");
    if (!st) return std::unexpected(database_error(st.error()));
    if (auto r = st->run(); !r) return std::unexpected(database_error(r.error()));
    return static_cast<std::uint64_t>(db_->changes());
}

Result<core::Page<Track>> Library::tracks(const Filter& filter, std::span<const Order> order,
                                         const std::optional<core::Cursor>& after, std::size_t limit) {
    std::vector<media::Order> orders;
    for (const auto& o : order) orders.push_back(media::Order{int(o.field), o.descending});
    const auto conds = conditions(filter);
    auto built = media::items(schema(), track_columns, track_column_count, conds, orders, after, limit);
    if (!built) return std::unexpected(Error{built.error()});
    auto st = db_->prepare(built->sql);
    if (!st) return std::unexpected(database_error(st.error()));
    media::bind_all(*st, built->binds);
    core::Page<Track> page;
    while (true) {
        auto row = st->step();
        if (!row) return std::unexpected(database_error(row.error()));
        if (!*row) break;
        page.items.push_back(read_track(*st));
        if (page.items.size() == limit) page.next = media::cursor_from(*st, *built, page.items.back().entry_id);
    }
    return page;
}

Result<core::Page<Group>> Library::groups(std::span<const Field> by, const Filter& filter, GroupOrder order, bool descending,
                                         const std::optional<core::Cursor>& after, std::size_t limit) {
    std::vector<int> fields;
    for (Field f : by) fields.push_back(int(f));
    const auto conds = conditions(filter);
    auto built = media::groups(schema(), fields, group_aggregates, group_aggregate_count, conds, order_sql(order), descending,
                               after, limit);
    if (!built) return std::unexpected(Error{built.error()});
    auto st = db_->prepare(built->sql);
    if (!st) return std::unexpected(database_error(st.error()));
    media::bind_all(*st, built->binds);
    core::Page<Group> page;
    const int n = static_cast<int>(fields.size());
    while (true) {
        auto row = st->step();
        if (!row) return std::unexpected(database_error(row.error()));
        if (!*row) break;
        Group g;
        for (int i = 0; i < n; ++i) g.values.push_back(st->column_text(i));
        g.tracks = static_cast<std::uint64_t>(st->column_int64(n));
        g.duration_ms = st->column_int64(n + 1);
        g.added_first_ns = st->column_int64(n + 2);
        g.added_last_ns = st->column_int64(n + 3);
        g.year_min = static_cast<int>(st->column_int64(n + 4));
        g.year_max = static_cast<int>(st->column_int64(n + 5));
        g.cover_entry = st->column_int64(n + 6);
        g.any_online = st->column_int64(n + 7) != 0;
        page.items.push_back(std::move(g));
        if (page.items.size() == limit) page.next = media::cursor_from(*st, *built, 0);
    }
    return page;
}

Result<std::uint64_t> Library::count(const Filter& filter) {
    const auto conds = conditions(filter);
    return single_count(*db_, media::count(schema(), conds));
}

Result<std::uint64_t> Library::group_count(std::span<const Field> by, const Filter& filter) {
    std::vector<int> fields;
    for (Field f : by) fields.push_back(int(f));
    const auto conds = conditions(filter);
    return single_count(*db_, media::group_count(schema(), fields, conds));
}

Result<Track> Library::track(EntryId entry) {
    const Filter filter{{Condition{Field::Entry, Value{entry}}}};
    auto page = tracks(filter, {}, std::nullopt, 1);
    if (!page) return std::unexpected(page.error());
    if (page->items.empty()) return std::unexpected(Error{"no such track"});
    return page->items.front();
}

}
