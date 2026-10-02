#pragma once

// Builds the generic filter / order / group queries of the audio and video engines
// (features/media_listing.md §5). Each engine describes its fields as SQL once; this
// turns a request into one statement with keyset paging. Field ids are the engine's
// enum values. SQL comes only from these fixed descriptions, never from caller strings.

#include <hoardor/core/page.hpp>
#include <hoardor/db/database.hpp>

#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace hoardor::media {

using Value = std::variant<std::int64_t, std::string>;

struct FieldSql {
    std::string value{};    // SQL for the value shown to the user (e.g. "t.album_artist")
    std::string key{};      // SQL for filtering, ordering, grouping ("" = not orderable)
    bool text = false;      // a text key (cursors hold text)
    bool normalized = true; // for text keys: the column holds core::sort_key(value), so filter values get it too
    int names_kind = 0;     // > 0: several values per item, in the link/names tables with this kind
    std::string link_extra{}; // extra condition on the link row, with {l} for its alias (e.g. "{l}.role = 2")
    bool broad = false;     // a filter on it keeps most rows (category, root): group index hints still apply
};

struct Schema {
    std::string table;         // the engine's table with its alias, e.g. "audio_tracks t"
    std::string joins;         // the joins after it; aliases e (file_entries), r (file_roots)
    std::string id;            // the item id, e.g. "t.entry_id"
    std::string base_where;    // always applied (e.g. readable rows only)
    std::string link_table;    // (entry_id, name_id, …)
    std::string names_table;   // (id, kind, name, key)
    std::map<int, FieldSql> fields;
    // The index that returns groups in order, per group-by field list. SQLite treats GROUP BY
    // columns as a set and may stream them from another index (e.g. (album, artist) for an
    // artist-then-album request), then sort every group, so it can't stop after a page.
    // Used only when groups are ordered by their values and every filter is `broad`; a narrow
    // filter (a genre, a year) is better served by its own index.
    std::map<std::vector<int>, std::string> group_indexes;
};

struct Condition {
    int field = 0;
    Value value;
};

struct Order {
    int field = 0;
    bool descending = false;
};

// One statement and its parameters, ready to prepare and bind.
struct Built {
    std::string sql;
    std::vector<Value> binds;
    std::size_t key_column = 0;         // where the keyset columns start in each row
    std::vector<bool> key_is_text;      // their types, for reading them back into a cursor
};

using BuildResult = std::expected<Built, std::string>;

// SELECT <columns>, <order keys…>, <id> … ORDER BY the orders then the id, after the cursor.
// column_count: how many columns `columns` lists (the keyset columns follow them).
BuildResult items(const Schema& schema, std::string_view columns, std::size_t column_count,
                  std::span<const Condition> filter, std::span<const Order> order,
                  const std::optional<core::Cursor>& after, std::size_t limit);

// SELECT <group values…>, <aggregates>, <order value>, <group keys…> … GROUP BY the group keys.
// order_sql: an aggregate to order by ("" = order by the keys); descending applies to all.
// aggregate_count: how many columns `aggregates` lists.
BuildResult groups(const Schema& schema, std::span<const int> by, std::string_view aggregates,
                   std::size_t aggregate_count, std::span<const Condition> filter, std::string_view order_sql,
                   bool descending, const std::optional<core::Cursor>& after, std::size_t limit);

BuildResult count(const Schema& schema, std::span<const Condition> filter);
BuildResult group_count(const Schema& schema, std::span<const int> by, std::span<const Condition> filter);

void bind_all(db::Statement& statement, const std::vector<Value>& values);

// The cursor for the row the statement is on (its keyset columns), and the id for items.
core::Cursor cursor_from(const db::Statement& statement, const Built& built, std::int64_t id);

}
