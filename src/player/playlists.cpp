// player::Library's playlists and liked songs (features/playlists.md). Music only: an entry joins
// a playlist only if the audio engine read it as a track (a read-only join on audio_tracks).

#include <hoardor/player/player.hpp>

#include "core/text.hpp"

#include <algorithm>
#include <string>

namespace hoardor::player {

namespace {

Error database_error(const db::Error& error) { return Error{error.message}; }

// Unique-name key: trimmed, ASCII case folded (sort_key without articles; equal names, equal keys).
std::string name_key(std::string_view name) { return core::sort_key(core::trim(name), {}); }

constexpr std::string_view playlist_select =
    "SELECT p.id, p.name, p.pinned, p.created_ns, p.updated_ns, COUNT(i.id), COALESCE(SUM(a.duration_ms), 0) "
    "FROM player_playlists p LEFT JOIN player_playlist_items i ON i.playlist_id = p.id "
    "LEFT JOIN audio_tracks a ON a.entry_id = i.entry_id ";

Playlist read_playlist(const db::Statement& st) {
    Playlist p;
    p.id = st.column_int64(0);
    p.name = st.column_text(1);
    p.pinned = st.column_int64(2) != 0;
    p.created_ns = st.column_int64(3);
    p.updated_ns = st.column_int64(4);
    p.tracks = static_cast<std::uint64_t>(st.column_int64(5));
    p.duration_ms = st.column_int64(6);
    return p;
}

std::string json_ids(std::span<const std::int64_t> ids) {
    std::string out = "[";
    for (std::size_t i = 0; i < ids.size(); ++i) out += (i ? "," : "") + std::to_string(ids[i]);
    return out + "]";
}

// The single int64 a query returns (or `fallback` when there's no row or it's NULL).
Result<std::int64_t> one_int(db::Database& db, std::string_view sql, std::int64_t bind1, std::int64_t fallback) {
    auto st = db.prepare(sql);
    if (!st) return std::unexpected(database_error(st.error()));
    st->bind(1, bind1);
    auto row = st->step();
    if (!row) return std::unexpected(database_error(row.error()));
    if (!*row || st->column_is_null(0)) return fallback;
    return st->column_int64(0);
}

}  // namespace

Result<std::vector<Playlist>> Library::playlists() {
    auto st = db_->prepare(std::string(playlist_select) + "GROUP BY p.id ORDER BY p.pinned DESC, p.sort, p.id");
    if (!st) return std::unexpected(database_error(st.error()));
    std::vector<Playlist> out;
    while (true) {
        auto row = st->step();
        if (!row) return std::unexpected(database_error(row.error()));
        if (!*row) break;
        out.push_back(read_playlist(*st));
    }
    return out;
}

Result<Playlist> Library::playlist(PlaylistId id) {
    auto st = db_->prepare(std::string(playlist_select) + "WHERE p.id = ? GROUP BY p.id");
    if (!st) return std::unexpected(database_error(st.error()));
    st->bind(1, id);
    auto row = st->step();
    if (!row) return std::unexpected(database_error(row.error()));
    if (!*row) return std::unexpected(Error{"no such playlist"});
    return read_playlist(*st);
}

namespace {

// A name the user can have: not empty, and not another playlist's (ignoring ASCII case).
Result<std::string> checked_name(db::Database& db, std::string_view name, PlaylistId self) {
    const std::string trimmed = core::trim(name);
    if (trimmed.empty()) return std::unexpected(Error{"a playlist needs a name"});
    auto st = db.prepare("SELECT id FROM player_playlists WHERE name_key = ? AND id <> ?");
    if (!st) return std::unexpected(database_error(st.error()));
    st->bind(1, name_key(trimmed)).bind(2, self);
    auto row = st->step();
    if (!row) return std::unexpected(database_error(row.error()));
    if (*row) return std::unexpected(Error{"there's already a playlist named \"" + trimmed + "\""});
    return trimmed;
}

}  // namespace

Result<PlaylistId> Library::create_playlist(std::string_view name, std::int64_t now_ns) {
    auto checked = checked_name(*db_, name, 0);
    if (!checked) return std::unexpected(checked.error());
    auto top = one_int(*db_, "SELECT MIN(sort) FROM player_playlists WHERE pinned = ?", 0, 1);
    if (!top) return std::unexpected(top.error());
    auto st = db_->prepare("INSERT INTO player_playlists (name, name_key, pinned, sort, created_ns, updated_ns) VALUES (?, ?, 0, ?, ?, ?)");
    if (!st) return std::unexpected(database_error(st.error()));
    st->bind(1, *checked).bind(2, name_key(*checked)).bind(3, *top - 1).bind(4, now_ns).bind(5, now_ns);
    if (auto r = st->run(); !r) return std::unexpected(database_error(r.error()));
    return db_->last_insert_id();
}

Result<void> Library::rename_playlist(PlaylistId id, std::string_view name, std::int64_t now_ns) {
    if (auto p = playlist(id); !p) return std::unexpected(p.error());
    auto checked = checked_name(*db_, name, id);
    if (!checked) return std::unexpected(checked.error());
    auto st = db_->prepare("UPDATE player_playlists SET name = ?, name_key = ?, updated_ns = ? WHERE id = ?");
    if (!st) return std::unexpected(database_error(st.error()));
    st->bind(1, *checked).bind(2, name_key(*checked)).bind(3, now_ns).bind(4, id);
    if (auto r = st->run(); !r) return std::unexpected(database_error(r.error()));
    return {};
}

Result<void> Library::delete_playlist(PlaylistId id) {
    auto st = db_->prepare("DELETE FROM player_playlists WHERE id = ?");
    if (!st) return std::unexpected(database_error(st.error()));
    st->bind(1, id);
    if (auto r = st->run(); !r) return std::unexpected(database_error(r.error()));
    return {};
}

Result<void> Library::set_pinned(PlaylistId id, bool pinned) {
    auto p = playlist(id);
    if (!p) return std::unexpected(p.error());
    if (p->pinned == pinned) return {};
    auto top = one_int(*db_, "SELECT MIN(sort) FROM player_playlists WHERE pinned = ?", pinned ? 1 : 0, 1);
    if (!top) return std::unexpected(top.error());
    auto st = db_->prepare("UPDATE player_playlists SET pinned = ?, sort = ? WHERE id = ?");
    if (!st) return std::unexpected(database_error(st.error()));
    st->bind(1, pinned ? 1 : 0).bind(2, *top - 1).bind(3, id);
    if (auto r = st->run(); !r) return std::unexpected(database_error(r.error()));
    return {};
}

namespace {

// Renumbers rows 0..n-1 in the given order (one statement per row, in one transaction).
Result<void> renumber(db::Database& db, std::string_view update_sql, const std::vector<std::int64_t>& ids) {
    auto tx = db::Transaction::begin(db);
    if (!tx) return std::unexpected(database_error(tx.error()));
    auto st = db.prepare(update_sql);
    if (!st) return std::unexpected(database_error(st.error()));
    for (std::size_t i = 0; i < ids.size(); ++i) {
        st->bind(1, static_cast<std::int64_t>(i)).bind(2, ids[i]);
        if (auto r = st->run(); !r) return std::unexpected(database_error(r.error()));
        st->reset();
    }
    if (auto r = tx->commit(); !r) return std::unexpected(database_error(r.error()));
    return {};
}

Result<std::vector<std::int64_t>> ids_of(db::Database& db, std::string_view sql, std::int64_t bind1) {
    auto st = db.prepare(sql);
    if (!st) return std::unexpected(database_error(st.error()));
    st->bind(1, bind1);
    std::vector<std::int64_t> out;
    while (true) {
        auto row = st->step();
        if (!row) return std::unexpected(database_error(row.error()));
        if (!*row) break;
        out.push_back(st->column_int64(0));
    }
    return out;
}

void move_to(std::vector<std::int64_t>& ids, std::int64_t id, std::size_t index) {
    ids.erase(std::remove(ids.begin(), ids.end(), id), ids.end());
    ids.insert(ids.begin() + static_cast<std::ptrdiff_t>(std::min(index, ids.size())), id);
}

}  // namespace

Result<void> Library::move_playlist(PlaylistId id, std::size_t index) {
    auto p = playlist(id);
    if (!p) return std::unexpected(p.error());
    auto group = ids_of(*db_, "SELECT id FROM player_playlists WHERE pinned = ? ORDER BY sort, id", p->pinned ? 1 : 0);
    if (!group) return std::unexpected(group.error());
    move_to(*group, id, index);
    return renumber(*db_, "UPDATE player_playlists SET sort = ? WHERE id = ?", *group);
}

Result<Added> Library::add_to_playlist(PlaylistId id, std::span<const EntryId> entries, std::int64_t now_ns) {
    if (auto p = playlist(id); !p) return std::unexpected(p.error());
    Added out;
    if (entries.empty()) return out;
    auto tx = db::Transaction::begin(*db_);
    if (!tx) return std::unexpected(database_error(tx.error()));
    auto last = one_int(*db_, "SELECT MAX(position) FROM player_playlist_items WHERE playlist_id = ?", id, -1);
    if (!last) return std::unexpected(last.error());
    std::int64_t position = *last;
    auto present = db_->prepare("SELECT 1 FROM player_playlist_items WHERE playlist_id = ? AND entry_id = ?");
    auto track = db_->prepare("SELECT 1 FROM audio_tracks WHERE entry_id = ? AND read_error = ''");
    auto insert = db_->prepare("INSERT INTO player_playlist_items (playlist_id, entry_id, position, added_ns) VALUES (?, ?, ?, ?)");
    if (!present || !track || !insert) {
        return std::unexpected(database_error(!present ? present.error() : !track ? track.error() : insert.error()));
    }
    for (EntryId e : entries) {
        present->reset();
        present->bind(1, id).bind(2, e);
        auto in = present->step();
        if (!in) return std::unexpected(database_error(in.error()));
        if (*in) {
            ++out.skipped;
            continue;
        }
        track->reset();
        track->bind(1, e);
        auto is_track = track->step();
        if (!is_track) return std::unexpected(database_error(is_track.error()));
        if (!*is_track) {
            ++out.not_tracks;
            continue;
        }
        insert->reset();
        insert->bind(1, id).bind(2, e).bind(3, ++position).bind(4, now_ns);
        if (auto r = insert->run(); !r) return std::unexpected(database_error(r.error()));
        ++out.added;
    }
    present->reset();
    track->reset();
    if (out.added > 0) {
        auto touch = db_->prepare("UPDATE player_playlists SET updated_ns = ? WHERE id = ?");
        if (!touch) return std::unexpected(database_error(touch.error()));
        touch->bind(1, now_ns).bind(2, id);
        if (auto r = touch->run(); !r) return std::unexpected(database_error(r.error()));
    }
    if (auto r = tx->commit(); !r) return std::unexpected(database_error(r.error()));
    return out;
}

Result<void> Library::remove_from_playlist(PlaylistId id, std::span<const PlaylistItemId> items, std::int64_t now_ns) {
    if (items.empty()) return {};
    auto st = db_->prepare("DELETE FROM player_playlist_items WHERE playlist_id = ? AND id IN (SELECT value FROM json_each(?))");
    if (!st) return std::unexpected(database_error(st.error()));
    const std::string ids = json_ids(items);
    st->bind(1, id).bind(2, std::string_view(ids));
    if (auto r = st->run(); !r) return std::unexpected(database_error(r.error()));
    if (db_->changes() > 0) {
        auto touch = db_->prepare("UPDATE player_playlists SET updated_ns = ? WHERE id = ?");
        if (!touch) return std::unexpected(database_error(touch.error()));
        touch->bind(1, now_ns).bind(2, id);
        if (auto r = touch->run(); !r) return std::unexpected(database_error(r.error()));
    }
    return {};
}

Result<void> Library::move_in_playlist(PlaylistId id, PlaylistItemId item, std::size_t index, std::int64_t now_ns) {
    auto rows = ids_of(*db_, "SELECT id FROM player_playlist_items WHERE playlist_id = ? ORDER BY position, id", id);
    if (!rows) return std::unexpected(rows.error());
    if (std::find(rows->begin(), rows->end(), item) == rows->end()) return std::unexpected(Error{"that row isn't in this playlist"});
    move_to(*rows, item, index);
    if (auto r = renumber(*db_, "UPDATE player_playlist_items SET position = ? WHERE id = ?", *rows); !r) return r;
    auto touch = db_->prepare("UPDATE player_playlists SET updated_ns = ? WHERE id = ?");
    if (!touch) return std::unexpected(database_error(touch.error()));
    touch->bind(1, now_ns).bind(2, id);
    if (auto r = touch->run(); !r) return std::unexpected(database_error(r.error()));
    return {};
}

Result<core::Page<PlaylistItem>> Library::playlist_items(PlaylistId id, const std::optional<core::Cursor>& after, std::size_t limit) {
    // Keyset on (position, id): the cursor's key is the position, its id the row's.
    auto st = db_->prepare("SELECT id, entry_id, position, added_ns FROM player_playlist_items WHERE playlist_id = ? "
                           "AND (position > ? OR (position = ? AND id > ?)) ORDER BY position, id LIMIT ?");
    if (!st) return std::unexpected(database_error(st.error()));
    const std::int64_t pos = after ? std::stoll(after->key) : INT64_MIN;
    st->bind(1, id).bind(2, pos).bind(3, pos).bind(4, after ? after->id : INT64_MIN).bind(5, static_cast<std::int64_t>(limit));
    core::Page<PlaylistItem> page;
    while (true) {
        auto row = st->step();
        if (!row) return std::unexpected(database_error(row.error()));
        if (!*row) break;
        page.items.push_back({st->column_int64(0), st->column_int64(1), st->column_int64(2), st->column_int64(3)});
    }
    if (page.items.size() == limit && limit > 0) page.next = core::Cursor{std::to_string(page.items.back().position), page.items.back().id};
    return page;
}

Result<std::vector<EntryId>> Library::playlist_entries(PlaylistId id) {
    return ids_of(*db_, "SELECT entry_id FROM player_playlist_items WHERE playlist_id = ? ORDER BY position, id", id);
}

Result<std::vector<PlaylistId>> Library::playlists_with(EntryId entry) {
    return ids_of(*db_, "SELECT DISTINCT playlist_id FROM player_playlist_items WHERE entry_id = ?", entry);
}

namespace {

constexpr std::string_view liked_from =
    " FROM player_items p JOIN audio_tracks a ON a.entry_id = p.entry_id AND a.read_error = '' WHERE p.liked_ns > 0";

}

Result<core::Page<Liked>> Library::liked(const std::optional<core::Cursor>& after, std::size_t limit) {
    // Newest first; keyset on (liked_ns, entry) descending.
    auto st = db_->prepare("SELECT p.entry_id, p.liked_ns" + std::string(liked_from) +
                           " AND (p.liked_ns < ? OR (p.liked_ns = ? AND p.entry_id < ?)) ORDER BY p.liked_ns DESC, p.entry_id DESC LIMIT ?");
    if (!st) return std::unexpected(database_error(st.error()));
    const std::int64_t ns = after ? std::stoll(after->key) : INT64_MAX;
    st->bind(1, ns).bind(2, ns).bind(3, after ? after->id : INT64_MAX).bind(4, static_cast<std::int64_t>(limit));
    core::Page<Liked> page;
    while (true) {
        auto row = st->step();
        if (!row) return std::unexpected(database_error(row.error()));
        if (!*row) break;
        page.items.push_back({st->column_int64(0), st->column_int64(1)});
    }
    if (page.items.size() == limit && limit > 0) page.next = core::Cursor{std::to_string(page.items.back().liked_ns), page.items.back().entry};
    return page;
}

Result<std::uint64_t> Library::liked_count() {
    auto st = db_->prepare("SELECT COUNT(*)" + std::string(liked_from));
    if (!st) return std::unexpected(database_error(st.error()));
    auto row = st->step();
    if (!row) return std::unexpected(database_error(row.error()));
    return static_cast<std::uint64_t>(st->column_int64(0));
}

Result<std::vector<EntryId>> Library::liked_entries() {
    auto st = db_->prepare("SELECT p.entry_id" + std::string(liked_from) + " ORDER BY p.liked_ns DESC, p.entry_id DESC");
    if (!st) return std::unexpected(database_error(st.error()));
    std::vector<EntryId> out;
    while (true) {
        auto row = st->step();
        if (!row) return std::unexpected(database_error(row.error()));
        if (!*row) break;
        out.push_back(st->column_int64(0));
    }
    return out;
}

}  // namespace hoardor::player
