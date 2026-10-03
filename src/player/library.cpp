// player::Library: player_items and player_settings (features/player.md §5–§7).

#include <hoardor/player/player.hpp>

#include <array>
#include <charconv>
#include <string_view>
#include <unordered_map>

namespace hoardor::player {

namespace {

constexpr std::string_view schema_v1 = R"sql(
CREATE TABLE player_items (
    entry_id INTEGER PRIMARY KEY REFERENCES file_entries(id) ON DELETE CASCADE,
    position_ms INTEGER NOT NULL DEFAULT 0,
    duration_ms INTEGER NOT NULL DEFAULT 0,
    viewed INTEGER NOT NULL DEFAULT 0,
    play_count INTEGER NOT NULL DEFAULT 0,
    last_played_ns INTEGER NOT NULL DEFAULT 0,
    liked_ns INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX player_items_recent ON player_items (last_played_ns);
CREATE INDEX player_items_liked ON player_items (liked_ns) WHERE liked_ns > 0;
CREATE TABLE player_settings (key TEXT PRIMARY KEY, value TEXT NOT NULL);
)sql";

constexpr std::array<db::Migration, 1> migrations{{{1, schema_v1}}};

Error database_error(const db::Error& error) { return Error{error.message}; }

constexpr std::string_view item_columns = "entry_id, position_ms, duration_ms, viewed, play_count, last_played_ns, liked_ns";

ItemState read_item(const db::Statement& st) {
    ItemState s;
    s.entry = st.column_int64(0);
    s.position_ms = st.column_int64(1);
    s.duration_ms = st.column_int64(2);
    s.viewed = st.column_int64(3) != 0;
    s.play_count = static_cast<int>(st.column_int64(4));
    s.last_played_ns = st.column_int64(5);
    s.liked_ns = st.column_int64(6);
    return s;
}

// One row per setting: its key, how to read and write it, and its limits.
struct IntSetting {
    std::string_view key;
    int Settings::*field;
    SettingLimits limits;
};
struct BoolSetting {
    std::string_view key;
    bool Settings::*field;
};
struct TextSetting {
    std::string_view key;
    std::string Settings::*field;
};

constexpr std::array int_settings{
    IntSetting{"read_ahead_mib", &Settings::read_ahead_mib, {1, 4096}},
    IntSetting{"read_ahead_seconds", &Settings::read_ahead_seconds, {1, 600}},
    IntSetting{"previous_restarts_after_seconds", &Settings::previous_restarts_after_seconds, {0, 60}},
    IntSetting{"progress_save_seconds", &Settings::progress_save_seconds, {1, 600}},
    IntSetting{"resume_min_seconds", &Settings::resume_min_seconds, {0, 3600}},
    IntSetting{"viewed_percent", &Settings::viewed_percent, {1, 100}},
    IntSetting{"play_count_percent", &Settings::play_count_percent, {1, 100}},
    IntSetting{"play_count_seconds", &Settings::play_count_seconds, {1, 3600}},
    IntSetting{"volume", &Settings::volume, {0, 100}},
};
constexpr std::array bool_settings{
    BoolSetting{"hardware_decoding", &Settings::hardware_decoding},
    BoolSetting{"subtitles_on", &Settings::subtitles_on},
    BoolSetting{"resume_audio", &Settings::resume_audio},
    BoolSetting{"muted", &Settings::muted},
};
constexpr std::array text_settings{
    TextSetting{"audio_languages", &Settings::audio_languages},
    TextSetting{"subtitle_languages", &Settings::subtitle_languages},
};

}  // namespace

Result<Library> Library::open(db::Database& database) {
    if (auto r = db::migrate(database, "player", migrations); !r) return std::unexpected(database_error(r.error()));
    return Library(database);
}

Result<ItemState> Library::state(EntryId entry) {
    auto st = db_->prepare("SELECT " + std::string(item_columns) + " FROM player_items WHERE entry_id = ?");
    if (!st) return std::unexpected(database_error(st.error()));
    st->bind(1, entry);
    auto row = st->step();
    if (!row) return std::unexpected(database_error(row.error()));
    if (!*row) return ItemState{.entry = entry};
    return read_item(*st);
}

Result<std::vector<ItemState>> Library::states(std::span<const EntryId> entries) {
    std::vector<ItemState> out(entries.size());
    if (entries.empty()) return out;
    // One query for the page: the ids as a JSON array, so the statement has a single parameter.
    std::string ids = "[";
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (i) ids += ',';
        ids += std::to_string(entries[i]);
    }
    ids += ']';
    auto st = db_->prepare("SELECT " + std::string(item_columns) +
                           " FROM player_items WHERE entry_id IN (SELECT value FROM json_each(?))");
    if (!st) return std::unexpected(database_error(st.error()));
    st->bind(1, std::string_view(ids));
    std::unordered_map<EntryId, ItemState> found;
    while (true) {
        auto row = st->step();
        if (!row) return std::unexpected(database_error(row.error()));
        if (!*row) break;
        ItemState s = read_item(*st);
        found.emplace(s.entry, s);
    }
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto it = found.find(entries[i]);
        out[i] = it != found.end() ? it->second : ItemState{.entry = entries[i]};
    }
    return out;
}

Result<void> Library::set_liked(EntryId entry, bool liked, std::int64_t now_ns) {
    auto st = db_->prepare("INSERT INTO player_items (entry_id, liked_ns) VALUES (?, ?) "
                           "ON CONFLICT (entry_id) DO UPDATE SET liked_ns = excluded.liked_ns");
    if (!st) return std::unexpected(database_error(st.error()));
    st->bind(1, entry).bind(2, liked ? now_ns : std::int64_t{0});
    if (auto r = st->run(); !r) return std::unexpected(database_error(r.error()));
    return {};
}

Result<void> Library::save_position(EntryId entry, std::int64_t position_ms, std::int64_t duration_ms, bool viewed,
                                    std::int64_t now_ns) {
    auto st = db_->prepare(
        "INSERT INTO player_items (entry_id, position_ms, duration_ms, viewed, last_played_ns) VALUES (?, ?, ?, ?, ?) "
        "ON CONFLICT (entry_id) DO UPDATE SET position_ms = excluded.position_ms, duration_ms = excluded.duration_ms, "
        "viewed = MAX(viewed, excluded.viewed), last_played_ns = excluded.last_played_ns");
    if (!st) return std::unexpected(database_error(st.error()));
    st->bind(1, entry).bind(2, position_ms).bind(3, duration_ms).bind(4, viewed ? 1 : 0).bind(5, now_ns);
    if (auto r = st->run(); !r) return std::unexpected(database_error(r.error()));
    return {};
}

Result<void> Library::count_play(EntryId entry, std::int64_t now_ns) {
    auto st = db_->prepare("INSERT INTO player_items (entry_id, play_count, last_played_ns) VALUES (?, 1, ?) "
                           "ON CONFLICT (entry_id) DO UPDATE SET play_count = play_count + 1, "
                           "last_played_ns = excluded.last_played_ns");
    if (!st) return std::unexpected(database_error(st.error()));
    st->bind(1, entry).bind(2, now_ns);
    if (auto r = st->run(); !r) return std::unexpected(database_error(r.error()));
    return {};
}

Result<Settings> Library::load_settings() {
    Settings s = Settings::defaults();
    auto st = db_->prepare("SELECT key, value FROM player_settings");
    if (!st) return std::unexpected(database_error(st.error()));
    while (true) {
        auto row = st->step();
        if (!row) return std::unexpected(database_error(row.error()));
        if (!*row) break;
        const std::string key = st->column_text(0);
        const std::string value = st->column_text(1);
        for (const auto& setting : int_settings) {
            if (key != setting.key) continue;
            int n = 0;
            const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), n);
            if (ec == std::errc() && end == value.data() + value.size() && n >= setting.limits.min &&
                n <= setting.limits.max) {
                s.*setting.field = n;
            }
        }
        for (const auto& setting : bool_settings) {
            if (key == setting.key && (value == "0" || value == "1")) s.*setting.field = value == "1";
        }
        for (const auto& setting : text_settings) {
            if (key == setting.key) s.*setting.field = value;
        }
    }
    return s;
}

Result<void> Library::save_settings(const Settings& settings) {
    for (const auto& setting : int_settings) {
        const int n = settings.*setting.field;
        if (n < setting.limits.min || n > setting.limits.max) {
            return std::unexpected(Error{std::string(setting.key) + " must be between " +
                                         std::to_string(setting.limits.min) + " and " +
                                         std::to_string(setting.limits.max)});
        }
    }
    auto tx = db::Transaction::begin(*db_);
    if (!tx) return std::unexpected(database_error(tx.error()));
    auto st = db_->prepare("INSERT INTO player_settings (key, value) VALUES (?, ?) "
                           "ON CONFLICT (key) DO UPDATE SET value = excluded.value");
    if (!st) return std::unexpected(database_error(st.error()));
    const auto put = [&](std::string_view key, std::string_view value) -> Result<void> {
        st->bind(1, key).bind(2, value);
        auto r = st->run();
        st->reset();
        if (!r) return std::unexpected(database_error(r.error()));
        return {};
    };
    for (const auto& setting : int_settings) {
        if (auto r = put(setting.key, std::to_string(settings.*setting.field)); !r) return r;
    }
    for (const auto& setting : bool_settings) {
        if (auto r = put(setting.key, settings.*setting.field ? "1" : "0"); !r) return r;
    }
    for (const auto& setting : text_settings) {
        if (auto r = put(setting.key, settings.*setting.field); !r) return r;
    }
    if (auto r = tx->commit(); !r) return std::unexpected(database_error(r.error()));
    return {};
}

}
