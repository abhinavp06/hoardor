#include "media/query.hpp"

#include "core/text.hpp"

#include <algorithm>
#include <charconv>

namespace hoardor::media {

namespace {

// Cursor keys are a list of typed values: "i<number>;" or "s<length>:<bytes>".
std::string encode(const std::vector<Value>& values) {
    std::string out;
    for (const Value& v : values) {
        if (const auto* i = std::get_if<std::int64_t>(&v)) {
            out += "i" + std::to_string(*i) + ";";
        } else {
            const auto& s = std::get<std::string>(v);
            out += "s" + std::to_string(s.size()) + ":" + s;
        }
    }
    return out;
}

std::optional<std::vector<Value>> decode(std::string_view text) {
    std::vector<Value> out;
    while (!text.empty()) {
        const char type = text.front();
        text.remove_prefix(1);
        if (type == 'i') {
            const auto end = text.find(';');
            if (end == std::string_view::npos) return std::nullopt;
            std::int64_t value = 0;
            const auto [ptr, ec] = std::from_chars(text.data(), text.data() + end, value);
            if (ec != std::errc{} || ptr != text.data() + end) return std::nullopt;
            out.emplace_back(value);
            text.remove_prefix(end + 1);
        } else if (type == 's') {
            const auto colon = text.find(':');
            if (colon == std::string_view::npos) return std::nullopt;
            std::size_t length = 0;
            const auto [ptr, ec] = std::from_chars(text.data(), text.data() + colon, length);
            if (ec != std::errc{} || ptr != text.data() + colon || colon + 1 + length > text.size()) return std::nullopt;
            out.emplace_back(std::string(text.substr(colon + 1, length)));
            text.remove_prefix(colon + 1 + length);
        } else {
            return std::nullopt;
        }
    }
    return out;
}

std::string replace_alias(std::string text, std::string_view alias) {
    for (std::size_t at = text.find("{l}"); at != std::string::npos; at = text.find("{l}", at)) {
        text.replace(at, 3, alias);
    }
    return text;
}

std::expected<const FieldSql*, std::string> field_of(const Schema& schema, int id) {
    const auto it = schema.fields.find(id);
    if (it == schema.fields.end()) return std::unexpected("unknown field");
    return &it->second;
}

// A filter value as the key column stores it.
std::expected<Value, std::string> key_value(const FieldSql& field, const Value& value) {
    if (field.text || field.names_kind > 0) {
        const std::string text = std::holds_alternative<std::string>(value) ? std::get<std::string>(value)
                                                                            : std::to_string(std::get<std::int64_t>(value));
        return Value{field.names_kind > 0 || field.normalized ? core::sort_key(text) : text};
    }
    if (const auto* i = std::get_if<std::int64_t>(&value)) return Value{*i};
    const std::string text = core::trim(std::get<std::string>(value));
    std::int64_t number = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), number);
    if (ec != std::errc{} || ptr != text.data() + text.size()) return std::unexpected("'" + text + "' is not a number");
    return Value{number};
}

// The filter as joins (several-valued fields: one link row for the wanted name, so SQLite can
// start from that name's rows) and WHERE conditions (ANDed, without the WHERE keyword).
struct FilterSql {
    std::string joins;
    std::string where;
};

std::expected<FilterSql, std::string> filter_sql(const Schema& schema, std::span<const Condition> filter,
                                                 std::vector<Value>& join_binds, std::vector<Value>& where_binds) {
    FilterSql out{"", schema.base_where.empty() ? "1" : schema.base_where};
    int n = 0;
    for (const Condition& c : filter) {
        auto field = field_of(schema, c.field);
        if (!field) return std::unexpected(field.error());
        auto value = key_value(**field, c.value);
        if (!value) return std::unexpected(value.error());
        if ((*field)->names_kind > 0) {
            const std::string l = "fl" + std::to_string(n++);
            out.joins += " JOIN " + schema.link_table + " " + l + " ON " + l + ".entry_id = " + schema.id + " AND " + l +
                         ".name_id = (SELECT id FROM " + schema.names_table + " WHERE kind = " +
                         std::to_string((*field)->names_kind) + " AND key = ?)" +
                         ((*field)->link_extra.empty() ? "" : " AND " + replace_alias((*field)->link_extra, l));
            join_binds.push_back(std::move(*value));
        } else {
            if ((*field)->key.empty()) return std::unexpected("this field can't be filtered");
            out.where += " AND " + (*field)->key + " = ?";
            where_binds.push_back(std::move(*value));
        }
    }
    return out;
}

void append(std::vector<Value>& to, std::vector<Value>&& from) {
    for (auto& v : from) to.push_back(std::move(v));
}

// Keyset condition "row comes after the cursor" for keys with per-key directions:
// (k0 > ?) OR (k0 = ? AND k1 > ?) OR … (with < for descending keys).
std::string after_sql(const std::vector<std::string>& keys, const std::vector<bool>& descending,
                      const std::vector<Value>& values, std::vector<Value>& binds) {
    // All in one direction: a row-value comparison, which SQLite turns into an index seek.
    if (std::all_of(descending.begin(), descending.end(), [&](bool d) { return d == descending.front(); })) {
        std::string lhs = "(", rhs = "(";
        for (std::size_t i = 0; i < keys.size(); ++i) {
            lhs += (i ? ", " : "") + keys[i];
            rhs += i ? ", ?" : "?";
            binds.push_back(values[i]);
        }
        return lhs + ") " + (descending.front() ? "<" : ">") + " " + rhs + ")";
    }
    std::string sql = "(";
    for (std::size_t i = 0; i < keys.size(); ++i) {
        if (i) sql += " OR ";
        sql += "(";
        for (std::size_t j = 0; j < i; ++j) {
            sql += keys[j] + " = ? AND ";
            binds.push_back(values[j]);
        }
        sql += keys[i] + (descending[i] ? " < ?" : " > ?") + ")";
        binds.push_back(values[i]);
    }
    return sql + ")";
}

}

BuildResult items(const Schema& schema, std::string_view columns, std::size_t column_count,
                  std::span<const Condition> filter, std::span<const Order> order,
                  const std::optional<core::Cursor>& after, std::size_t limit) {
    Built built;
    built.key_column = column_count;
    std::vector<Value> where_binds;
    auto filter_parts = filter_sql(schema, filter, built.binds, where_binds);
    if (!filter_parts) return std::unexpected(filter_parts.error());
    append(built.binds, std::move(where_binds));

    std::vector<std::string> keys;
    std::vector<bool> descending;
    for (const Order& o : order) {
        auto field = field_of(schema, o.field);
        if (!field) return std::unexpected(field.error());
        if ((*field)->key.empty()) return std::unexpected("this field can't be sorted on");
        keys.push_back((*field)->key);
        descending.push_back(o.descending);
        built.key_is_text.push_back((*field)->text || (*field)->names_kind > 0);
    }
    keys.push_back(schema.id);
    descending.push_back(order.empty() ? false : order.back().descending);

    std::string sql = "SELECT " + std::string(columns);
    for (const auto& k : keys) sql += ", " + k;
    sql += " FROM " + schema.table + schema.joins + filter_parts->joins + " WHERE " + filter_parts->where;
    if (after) {
        auto values = decode(after->key);
        if (!values || values->size() != keys.size() - 1) return std::unexpected("this cursor belongs to another query");
        values->push_back(after->id);
        sql += " AND " + after_sql(keys, descending, *values, built.binds);
    }
    sql += " ORDER BY ";
    for (std::size_t i = 0; i < keys.size(); ++i) sql += (i ? ", " : "") + keys[i] + (descending[i] ? " DESC" : "");
    sql += " LIMIT ?";
    built.binds.emplace_back(static_cast<std::int64_t>(limit));
    built.sql = std::move(sql);
    return built;
}

BuildResult groups(const Schema& schema, std::span<const int> by, std::string_view aggregates,
                   std::size_t aggregate_count, std::span<const Condition> filter, std::string_view order_sql,
                   bool descending, const std::optional<core::Cursor>& after, std::size_t limit) {
    if (by.empty()) return std::unexpected("group by at least one field");
    Built built;
    built.key_column = by.size() + aggregate_count;
    std::string joins, values, keys_select;
    std::vector<std::string> keys;
    for (std::size_t i = 0; i < by.size(); ++i) {
        auto field = field_of(schema, by[i]);
        if (!field) return std::unexpected(field.error());
        std::string key, value;
        if ((*field)->names_kind > 0) {
            const std::string l = "gl" + std::to_string(i), nm = "gn" + std::to_string(i);
            joins += " JOIN " + schema.link_table + " " + l + " ON " + l + ".entry_id = " + schema.id + " JOIN " +
                     schema.names_table + " " + nm + " ON " + nm + ".id = " + l + ".name_id AND " + nm +
                     ".kind = " + std::to_string((*field)->names_kind) +
                     ((*field)->link_extra.empty() ? "" : " AND " + replace_alias((*field)->link_extra, l));
            key = nm + ".key";
            value = "MIN(" + nm + ".name)";
        } else {
            if ((*field)->key.empty()) return std::unexpected("this field can't be grouped on");
            key = (*field)->key;
            value = (*field)->text ? "MIN(" + (*field)->value + ")" : key;
        }
        values += (i ? ", " : "") + value;
        keys.push_back(key);
        built.key_is_text.push_back((*field)->text || (*field)->names_kind > 0);
    }

    std::vector<std::string> sort = keys;
    if (!order_sql.empty()) {
        sort.insert(sort.begin(), std::string(order_sql));
        built.key_is_text.insert(built.key_is_text.begin(), false);
    }

    std::vector<Value> where_binds;
    auto filter_parts = filter_sql(schema, filter, built.binds, where_binds);
    if (!filter_parts) return std::unexpected(filter_parts.error());
    append(built.binds, std::move(where_binds));

    std::string sql = "SELECT " + values + ", " + std::string(aggregates);
    for (const auto& s : sort) sql += ", " + s;
    std::string table = schema.table;
    const bool broad_filter = std::all_of(filter.begin(), filter.end(), [&](const Condition& c) {
        const auto f = schema.fields.find(c.field);
        return f != schema.fields.end() && f->second.broad;
    });
    if (order_sql.empty() && broad_filter) {
        const auto hint = schema.group_indexes.find(std::vector<int>(by.begin(), by.end()));
        if (hint != schema.group_indexes.end()) table += " INDEXED BY " + hint->second;
    }
    std::optional<std::vector<Value>> cursor_values;
    if (after) {
        cursor_values = decode(after->key);
        if (!cursor_values || cursor_values->size() != sort.size()) return std::unexpected("this cursor belongs to another query");
    }
    sql += " FROM " + table + schema.joins + filter_parts->joins + joins + " WHERE " + filter_parts->where;
    // Ordered by the keys alone, "after the cursor" is a condition on the rows (an index seek);
    // ordered by an aggregate, it can only filter finished groups.
    if (cursor_values && order_sql.empty()) {
        sql += " AND " + after_sql(sort, std::vector<bool>(sort.size(), descending), *cursor_values, built.binds);
    }
    sql += " GROUP BY ";
    for (std::size_t i = 0; i < keys.size(); ++i) sql += (i ? ", " : "") + keys[i];
    if (cursor_values && !order_sql.empty()) {
        sql += " HAVING " + after_sql(sort, std::vector<bool>(sort.size(), descending), *cursor_values, built.binds);
    }
    sql += " ORDER BY ";
    for (std::size_t i = 0; i < sort.size(); ++i) sql += (i ? ", " : "") + sort[i] + (descending ? " DESC" : "");
    sql += " LIMIT ?";
    built.binds.emplace_back(static_cast<std::int64_t>(limit));
    built.sql = std::move(sql);
    return built;
}

BuildResult count(const Schema& schema, std::span<const Condition> filter) {
    Built built;
    std::vector<Value> where_binds;
    auto filter_parts = filter_sql(schema, filter, built.binds, where_binds);
    if (!filter_parts) return std::unexpected(filter_parts.error());
    append(built.binds, std::move(where_binds));
    built.sql = "SELECT COUNT(*) FROM " + schema.table + schema.joins + filter_parts->joins + " WHERE " + filter_parts->where;
    return built;
}

BuildResult group_count(const Schema& schema, std::span<const int> by, std::span<const Condition> filter) {
    auto inner = groups(schema, by, "1", 1, filter, "", false, std::nullopt, 0);
    if (!inner) return inner;
    // Drop the LIMIT of the inner statement and count its rows.
    inner->sql.erase(inner->sql.rfind(" ORDER BY "));
    inner->binds.pop_back();
    inner->sql = "SELECT COUNT(*) FROM (" + inner->sql + ")";
    return inner;
}

void bind_all(db::Statement& statement, const std::vector<Value>& values) {
    int index = 1;
    for (const Value& v : values) {
        if (const auto* i = std::get_if<std::int64_t>(&v)) statement.bind(index++, *i);
        else statement.bind(index++, std::string_view(std::get<std::string>(v)));
    }
}

core::Cursor cursor_from(const db::Statement& statement, const Built& built, std::int64_t id) {
    std::vector<Value> values;
    for (std::size_t i = 0; i < built.key_is_text.size(); ++i) {
        const int column = static_cast<int>(built.key_column + i);
        if (built.key_is_text[i]) values.emplace_back(statement.column_text(column));
        else values.emplace_back(statement.column_int64(column));
    }
    return core::Cursor{encode(values), id};
}

}
