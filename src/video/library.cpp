// video::Library: the video engine's tables and generic queries (features/media_listing.md §5, §6).

#include <hoardor/video/video.hpp>

#include "core/text.hpp"
#include "media/query.hpp"

#include <array>
#include <charconv>

namespace hoardor::video {

namespace {

Error database_error(const db::Error& e) { return Error{e.message}; }

constexpr std::string_view schema_v1 = R"sql(
CREATE TABLE video_items (
    entry_id INTEGER PRIMARY KEY REFERENCES file_entries(id) ON DELETE CASCADE,
    source_size INTEGER NOT NULL,
    source_mtime_ns INTEGER NOT NULL,
    read_error TEXT NOT NULL DEFAULT '',
    type INTEGER NOT NULL DEFAULT 1,           -- 1 movie, 2 episode
    title TEXT NOT NULL DEFAULT '', title_key TEXT NOT NULL DEFAULT '',
    show TEXT NOT NULL DEFAULT '', show_key TEXT NOT NULL DEFAULT '',
    season INTEGER NOT NULL DEFAULT -1, episode INTEGER NOT NULL DEFAULT 0,
    year INTEGER NOT NULL DEFAULT 0, date TEXT NOT NULL DEFAULT '', plot TEXT NOT NULL DEFAULT '',
    genre TEXT NOT NULL DEFAULT '', director TEXT NOT NULL DEFAULT '',   -- display lists, one per line
    duration_ms INTEGER NOT NULL DEFAULT 0, width INTEGER NOT NULL DEFAULT 0, height INTEGER NOT NULL DEFAULT 0,
    hdr TEXT NOT NULL DEFAULT '', video_codec TEXT NOT NULL DEFAULT '', frame_rate_milli INTEGER NOT NULL DEFAULT 0,
    audio_streams TEXT NOT NULL DEFAULT '', subtitle_streams TEXT NOT NULL DEFAULT '',   -- lines of lang\tcodec\tchannels\ttitle
    source INTEGER NOT NULL DEFAULT 1, from_name INTEGER NOT NULL DEFAULT 0,
    poster_entry INTEGER NOT NULL DEFAULT 0,   -- a companion image (file_entries.id), 0: none
    embedded_poster INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX video_items_title ON video_items (type, title_key, year);
CREATE INDEX video_items_show ON video_items (type, show_key, season, episode);
CREATE INDEX video_items_year ON video_items (year);
CREATE TABLE video_names (
    id INTEGER PRIMARY KEY,
    kind INTEGER NOT NULL,   -- 1 genre, 2 person
    name TEXT NOT NULL,
    key TEXT NOT NULL,
    UNIQUE (kind, key)
);
CREATE TABLE video_item_names (
    entry_id INTEGER NOT NULL REFERENCES video_items(entry_id) ON DELETE CASCADE,
    name_id INTEGER NOT NULL REFERENCES video_names(id),
    role INTEGER NOT NULL,   -- 1 genre, 2 director, 3 writer
    position INTEGER NOT NULL,
    PRIMARY KEY (entry_id, name_id, role)
) WITHOUT ROWID;
CREATE INDEX video_item_names_name ON video_item_names (name_id, role, entry_id);
)sql";

constexpr std::array<db::Migration, 1> migrations{{{1, schema_v1}}};

constexpr int genre_kind = 1, person_kind = 2;
constexpr int genre_role = 1, director_role = 2, writer_role = 3;

std::string join(const std::vector<std::string>& values) {
    std::string out;
    for (const auto& v : values) out += (out.empty() ? "" : "\n") + v;
    return out;
}

std::vector<std::string> split(const std::string& text, char separator = '\n') {
    std::vector<std::string> out;
    if (text.empty()) return out;
    std::size_t start = 0;
    while (true) {
        const auto end = text.find(separator, start);
        out.push_back(text.substr(start, end - start));
        if (end == std::string::npos) return out;
        start = end + 1;
    }
}

std::string streams_text(const std::vector<Stream>& streams) {
    std::vector<std::string> lines;
    const auto clean = [](std::string s) {
        for (char& c : s) {
            if (c == '\t' || c == '\n') c = ' ';
        }
        return s;
    };
    for (const auto& s : streams) {
        lines.push_back(clean(s.language) + "\t" + clean(s.codec) + "\t" + std::to_string(s.channels) + "\t" + clean(s.title));
    }
    return join(lines);
}

std::vector<Stream> streams_from(const std::string& text) {
    std::vector<Stream> out;
    for (const auto& line : split(text)) {
        auto parts = split(line, '\t');
        parts.resize(4);
        int channels = 0;
        std::from_chars(parts[2].data(), parts[2].data() + parts[2].size(), channels);
        out.push_back(Stream{parts[0], parts[1], channels, parts[3]});
    }
    return out;
}

const media::Schema& schema() {
    static const media::Schema s = [] {
        media::Schema m;
        m.table = "video_items t";
        m.joins = " JOIN file_entries e ON e.id = t.entry_id JOIN file_roots r ON r.id = e.root_id "
                  "LEFT JOIN file_entries p ON p.id = t.poster_entry";
        m.id = "t.entry_id";
        m.base_where = "t.read_error = ''";
        m.link_table = "video_item_names";
        m.names_table = "video_names";
        const auto text = [](std::string value, std::string key) { return media::FieldSql{.value = value, .key = key, .text = true}; };
        const auto number = [](std::string column) { return media::FieldSql{.value = column, .key = column}; };
        m.fields[int(Field::Type)] = media::FieldSql{.value = "t.type", .key = "t.type", .broad = true};
        m.fields[int(Field::Title)] = text("t.title", "t.title_key");
        m.fields[int(Field::Year)] = number("t.year");
        m.fields[int(Field::Genre)] = media::FieldSql{.value = "t.genre", .text = true, .names_kind = genre_kind,
                                                      .link_extra = "{l}.role = " + std::to_string(genre_role)};
        m.fields[int(Field::Director)] = media::FieldSql{.value = "t.director", .text = true, .names_kind = person_kind,
                                                         .link_extra = "{l}.role = " + std::to_string(director_role)};
        m.fields[int(Field::Show)] = text("t.show", "t.show_key");
        m.fields[int(Field::Season)] = number("t.season");
        m.fields[int(Field::Episode)] = number("t.episode");
        m.fields[int(Field::Duration)] = number("t.duration_ms");
        m.fields[int(Field::Height)] = number("t.height");
        m.fields[int(Field::Hdr)] = media::FieldSql{.value = "t.hdr", .key = "t.hdr", .text = true, .normalized = false};
        m.fields[int(Field::VideoCodec)] = media::FieldSql{.value = "t.video_codec", .key = "t.video_codec", .text = true, .normalized = false};
        m.fields[int(Field::Added)] = number("e.added_ns");
        m.fields[int(Field::Category)] = media::FieldSql{.value = "r.category_id", .key = "r.category_id", .broad = true};
        m.fields[int(Field::Root)] = media::FieldSql{.value = "e.root_id", .key = "e.root_id", .broad = true};
        m.fields[int(Field::Entry)] = number("t.entry_id");
        m.group_indexes[{int(Field::Title), int(Field::Year)}] = "video_items_title";
        m.group_indexes[{int(Field::Show)}] = "video_items_show";
        m.group_indexes[{int(Field::Show), int(Field::Season)}] = "video_items_show";
        return m;
    }();
    return s;
}

constexpr std::string_view item_columns =
    "t.entry_id, t.type, t.title, t.show, t.season, t.episode, t.year, t.date, t.plot, t.genre, t.director, t.duration_ms, "
    "t.width, t.height, t.hdr, t.video_codec, t.frame_rate_milli, t.audio_streams, t.subtitle_streams, t.source, "
    "t.from_name, COALESCE(p.id, 0), t.embedded_poster, e.root_id, r.status, e.added_ns, e.size";
constexpr std::size_t item_column_count = 27;

Item read_item(const db::Statement& st) {
    Item i;
    i.entry_id = st.column_int64(0);
    i.type = static_cast<Type>(st.column_int64(1));
    i.title = st.column_text(2);
    i.show = st.column_text(3);
    i.season = static_cast<int>(st.column_int64(4));
    i.episode = static_cast<int>(st.column_int64(5));
    i.year = static_cast<int>(st.column_int64(6));
    i.date = st.column_text(7);
    i.plot = st.column_text(8);
    i.genres = split(st.column_text(9));
    i.directors = split(st.column_text(10));
    i.duration_ms = st.column_int64(11);
    i.width = static_cast<int>(st.column_int64(12));
    i.height = static_cast<int>(st.column_int64(13));
    i.hdr = st.column_text(14);
    i.video_codec = st.column_text(15);
    i.frame_rate_milli = static_cast<int>(st.column_int64(16));
    i.audio = streams_from(st.column_text(17));
    i.subtitles = streams_from(st.column_text(18));
    i.source = static_cast<Source>(st.column_int64(19));
    i.from_name = st.column_int64(20) != 0;
    i.poster_entry = st.column_int64(21);
    i.has_embedded_poster = st.column_int64(22) != 0;
    i.root_id = st.column_int64(23);
    i.root_online = st.column_int64(24) == 1;
    i.added_ns = st.column_int64(25);
    i.size = static_cast<std::uint64_t>(st.column_int64(26));
    return i;
}

constexpr std::string_view group_aggregates =
    "COUNT(*), SUM(t.duration_ms), MIN(e.added_ns), MAX(e.added_ns), COALESCE(MIN(NULLIF(t.year, 0)), 0), MAX(t.year), "
    "MAX(t.height), MAX(t.hdr <> ''), COALESCE(MIN(p.id), 0), "
    "COALESCE(MIN(CASE WHEN t.embedded_poster THEN t.entry_id END), 0), MAX(r.status = 1)";
constexpr std::size_t group_aggregate_count = 11;

std::string_view order_sql(GroupOrder order) {
    switch (order) {
        case GroupOrder::AddedLast: return "MAX(e.added_ns)";
        case GroupOrder::Year: return "MAX(t.year)";
        case GroupOrder::Items: return "COUNT(*)";
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
    "FROM file_entries e JOIN file_roots r ON r.id = e.root_id LEFT JOIN video_items t ON t.entry_id = e.id "
    "WHERE e.kind = 2 AND e.unsettled = 0 AND r.status = 1 "
    "AND (t.entry_id IS NULL OR t.source_size <> e.size OR t.source_mtime_ns <> e.mtime_ns) "
    "AND (? IS NULL OR r.category_id = ?) AND (? IS NULL OR e.root_id = ?)";

}

Result<Library> Library::open(db::Database& database) {
    if (auto r = db::migrate(database, "video", migrations); !r) return std::unexpected(database_error(r.error()));
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

Result<void> Library::store(EntryId entry, std::int64_t source_size, std::int64_t source_mtime_ns, const VideoInfo& info,
                            EntryId poster_entry) {
    auto del = db_->prepare("DELETE FROM video_items WHERE entry_id = ?");  // also drops its name links
    if (!del) return std::unexpected(database_error(del.error()));
    del->bind(1, entry);
    if (auto r = del->run(); !r) return std::unexpected(database_error(r.error()));

    auto st = db_->prepare(
        "INSERT INTO video_items (entry_id, source_size, source_mtime_ns, type, title, title_key, show, show_key, season, "
        "episode, year, date, plot, genre, director, duration_ms, width, height, hdr, video_codec, frame_rate_milli, "
        "audio_streams, subtitle_streams, source, from_name, poster_entry, embedded_poster) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
    if (!st) return std::unexpected(database_error(st.error()));
    st->bind(1, entry)
        .bind(2, source_size)
        .bind(3, source_mtime_ns)
        .bind(4, static_cast<std::int64_t>(info.type))
        .bind(5, std::string_view(info.title))
        .bind(6, std::string_view(core::sort_key(info.title)))
        .bind(7, std::string_view(info.show))
        .bind(8, std::string_view(core::sort_key(info.show)))
        .bind(9, info.season)
        .bind(10, info.episode)
        .bind(11, info.year)
        .bind(12, std::string_view(info.date))
        .bind(13, std::string_view(info.plot))
        .bind(14, std::string_view(join(info.genres)))
        .bind(15, std::string_view(join(info.directors)))
        .bind(16, info.duration_ms)
        .bind(17, info.width)
        .bind(18, info.height)
        .bind(19, std::string_view(info.hdr))
        .bind(20, std::string_view(info.video_codec))
        .bind(21, info.frame_rate_milli)
        .bind(22, std::string_view(streams_text(info.audio)))
        .bind(23, std::string_view(streams_text(info.subtitles)))
        .bind(24, static_cast<std::int64_t>(info.source))
        .bind(25, info.from_name ? 1 : 0)
        .bind(26, poster_entry)
        .bind(27, info.has_embedded_poster ? 1 : 0);
    if (auto r = st->run(); !r) return std::unexpected(database_error(r.error()));

    auto add_name = db_->prepare("INSERT INTO video_names (kind, name, key) VALUES (?, ?, ?) ON CONFLICT (kind, key) DO NOTHING");
    auto find_name = db_->prepare("SELECT id FROM video_names WHERE kind = ? AND key = ?");
    auto link = db_->prepare("INSERT OR IGNORE INTO video_item_names (entry_id, name_id, role, position) VALUES (?, ?, ?, ?)");
    for (auto* s : {&add_name, &find_name, &link}) {
        if (!*s) return std::unexpected(database_error(s->error()));
    }
    const auto store_names = [&](int kind, int role, const std::vector<std::string>& names) -> Result<void> {
        for (std::size_t i = 0; i < names.size(); ++i) {
            const std::string k = core::sort_key(names[i]);
            if (k.empty()) continue;
            add_name->bind(1, kind).bind(2, std::string_view(names[i])).bind(3, std::string_view(k));
            if (auto r = add_name->run(); !r) return std::unexpected(database_error(r.error()));
            add_name->reset();
            find_name->bind(1, kind).bind(2, std::string_view(k));
            auto row = find_name->step();
            if (!row) return std::unexpected(database_error(row.error()));
            const std::int64_t name_id = find_name->column_int64(0);
            find_name->reset();
            link->bind(1, entry).bind(2, name_id).bind(3, role).bind(4, static_cast<std::int64_t>(i));
            if (auto r = link->run(); !r) return std::unexpected(database_error(r.error()));
            link->reset();
        }
        return {};
    };
    if (auto r = store_names(genre_kind, genre_role, info.genres); !r) return r;
    if (auto r = store_names(person_kind, director_role, info.directors); !r) return r;
    return store_names(person_kind, writer_role, info.writers);
}

Result<void> Library::store_error(EntryId entry, std::int64_t source_size, std::int64_t source_mtime_ns,
                                  std::string_view message) {
    auto del = db_->prepare("DELETE FROM video_items WHERE entry_id = ?");
    if (!del) return std::unexpected(database_error(del.error()));
    del->bind(1, entry);
    if (auto r = del->run(); !r) return std::unexpected(database_error(r.error()));
    auto st = db_->prepare("INSERT INTO video_items (entry_id, source_size, source_mtime_ns, read_error) VALUES (?, ?, ?, ?)");
    if (!st) return std::unexpected(database_error(st.error()));
    st->bind(1, entry).bind(2, source_size).bind(3, source_mtime_ns).bind(4, message.empty() ? std::string_view("unreadable") : message);
    if (auto r = st->run(); !r) return std::unexpected(database_error(r.error()));
    return {};
}

Result<std::uint64_t> Library::remove_unused_names() {
    auto st = db_->prepare("DELETE FROM video_names WHERE NOT EXISTS (SELECT 1 FROM video_item_names l WHERE l.name_id = video_names.id)");
    if (!st) return std::unexpected(database_error(st.error()));
    if (auto r = st->run(); !r) return std::unexpected(database_error(r.error()));
    return static_cast<std::uint64_t>(db_->changes());
}

Result<core::Page<Item>> Library::items(const Filter& filter, std::span<const Order> order,
                                       const std::optional<core::Cursor>& after, std::size_t limit) {
    std::vector<media::Order> orders;
    for (const auto& o : order) orders.push_back(media::Order{int(o.field), o.descending});
    const auto conds = conditions(filter);
    auto built = media::items(schema(), item_columns, item_column_count, conds, orders, after, limit);
    if (!built) return std::unexpected(Error{built.error()});
    auto st = db_->prepare(built->sql);
    if (!st) return std::unexpected(database_error(st.error()));
    media::bind_all(*st, built->binds);
    core::Page<Item> page;
    while (true) {
        auto row = st->step();
        if (!row) return std::unexpected(database_error(row.error()));
        if (!*row) break;
        page.items.push_back(read_item(*st));
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
        g.items = static_cast<std::uint64_t>(st->column_int64(n));
        g.duration_ms = st->column_int64(n + 1);
        g.added_first_ns = st->column_int64(n + 2);
        g.added_last_ns = st->column_int64(n + 3);
        g.year_min = static_cast<int>(st->column_int64(n + 4));
        g.year_max = static_cast<int>(st->column_int64(n + 5));
        g.max_height = static_cast<int>(st->column_int64(n + 6));
        g.any_hdr = st->column_int64(n + 7) != 0;
        g.poster_entry = st->column_int64(n + 8);
        g.embedded_poster_entry = st->column_int64(n + 9);
        g.any_online = st->column_int64(n + 10) != 0;
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

Result<Item> Library::item(EntryId entry) {
    const Filter filter{{Condition{Field::Entry, Value{entry}}}};
    auto page = items(filter, {}, std::nullopt, 1);
    if (!page) return std::unexpected(page.error());
    if (page->items.empty()) return std::unexpected(Error{"no such video"});
    return page->items.front();
}

}
