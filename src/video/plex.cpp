// video::PlexPosters: the posters Plex Media Server already downloaded, from its own database
// and files, read-only (features/posters.md §3).

#include <hoardor/video/video.hpp>

#include "file/text.hpp"

#include <algorithm>
#include <cstdlib>
#include <system_error>

namespace hoardor::video {

namespace fs = std::filesystem;

namespace {

constexpr int plex_movie = 1, plex_episode = 4;

fs::path database_in(const fs::path& plex_folder) {
    return plex_folder / "Plug-in Support" / "Databases" / "com.plexapp.plugins.library.db";
}

// Lowercase (ASCII) with '/' separators, so Plex's "E:\Movies\…" and hoardor's "E:/Movies/…" compare.
std::string normalized(std::string_view path) {
    std::string out = file::detail::ascii_lower(path);
    std::replace(out.begin(), out.end(), '\\', '/');
    return out;
}

// The last folder and the file name: "heat (1995)/heat.mkv". Survives another drive letter,
// a share's UNC path, or a moved library folder.
std::string key_of(std::string_view path) {
    const std::string n = normalized(path);
    const std::size_t last = n.rfind('/');
    if (last == std::string::npos || last == 0) return n;
    const std::size_t before = n.rfind('/', last - 1);
    return before == std::string::npos ? n : n.substr(before + 1);
}

bool is_file(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}

}

struct PlexPosters::State {
    db::Database plex;
    fs::path folder;
    db::Statement lookup;

    // A bundle: Plex's folder of everything it knows about one movie, show, or season.
    fs::path bundle(std::string_view hash, bool show) const {
        if (hash.size() < 2) return {};
        return folder / "Metadata" / (show ? "TV Shows" : "Movies") / std::string(hash.substr(0, 1)) /
               (std::string(hash.substr(1)) + ".bundle");
    }

    // The image a `user_thumb_url` names, if it's on disk; else any poster in the bundle.
    std::optional<fs::path> image(std::string_view url, std::string_view hash, bool show) const {
        const fs::path b = bundle(hash, show);
        const auto after = [&](std::string_view scheme) { return file::detail::from_utf8(url.substr(scheme.size())); };
        std::optional<fs::path> named;
        if (url.starts_with("metadata://") && !b.empty()) named = b / "Contents" / "_combined" / after("metadata://");
        else if (url.starts_with("upload://") && !b.empty()) named = b / "Uploads" / after("upload://");
        else if (url.starts_with("media://")) named = folder / "Media" / "localhost" / after("media://");
        if (named && is_file(*named)) return named;
        if (b.empty() || url.starts_with("http")) return std::nullopt;  // a web address: never fetched
        // The chosen file is gone (Plex refreshed): any poster it has for the same item.
        std::error_code ec;
        std::optional<fs::path> first;
        for (fs::directory_iterator it(b / "Contents" / "_combined" / "posters", ec), end; !ec && it != end; it.increment(ec)) {
            if (is_file(it->path()) && (!first || it->path() < *first)) first = it->path();
        }
        return first;
    }
};

std::optional<fs::path> PlexPosters::find_folder() {
    std::vector<fs::path> candidates;
    if (const char* local = std::getenv("LOCALAPPDATA"); local && *local) {
        candidates.push_back(file::detail::from_utf8(local) / "Plex Media Server");  // Windows
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        candidates.push_back(file::detail::from_utf8(home) / "Library" / "Application Support" / "Plex Media Server");  // macOS
    }
    candidates.emplace_back("/var/lib/plexmediaserver/Library/Application Support/Plex Media Server");  // Linux packages
    for (const fs::path& c : candidates) {
        if (is_file(database_in(c))) return c;
    }
    return std::nullopt;
}

Result<PlexPosters> PlexPosters::open(const fs::path& plex_folder) {
    const fs::path file = database_in(plex_folder);
    if (!is_file(file)) return std::unexpected(Error{"no Plex database in " + file::detail::to_utf8(plex_folder).text});
    auto plex = db::Database::open_read_only(file);
    if (!plex) return std::unexpected(Error{"opening Plex's database: " + plex.error().message});

    // One pass over Plex's tables into an indexed TEMP table (SQLite's temporary file, not
    // memory, and never Plex's file): per video file, its movie's or its show's and season's poster.
    if (auto r = plex->exec(R"sql(
CREATE TEMP TABLE hoardor_plex (
    key TEXT NOT NULL, path TEXT NOT NULL, kind INTEGER NOT NULL,
    hash TEXT, thumb TEXT, season_hash TEXT, season_thumb TEXT, show_hash TEXT, show_thumb TEXT);
)sql"); !r) {
        return std::unexpected(Error{"Plex: " + r.error().message});
    }
    auto read = plex->prepare(R"sql(
SELECT p.file, m.metadata_type, m.hash, m.user_thumb_url, se.hash, se.user_thumb_url, sh.hash, sh.user_thumb_url
FROM media_parts p
JOIN media_items i ON i.id = p.media_item_id
JOIN metadata_items m ON m.id = i.metadata_item_id
LEFT JOIN metadata_items se ON m.metadata_type = 4 AND se.id = m.parent_id
LEFT JOIN metadata_items sh ON se.id IS NOT NULL AND sh.id = se.parent_id
WHERE p.file IS NOT NULL AND p.file <> '' AND m.metadata_type IN (1, 4)
)sql");
    if (!read) return std::unexpected(Error{"Plex's database isn't what hoardor expects: " + read.error().message});
    auto insert = plex->prepare("INSERT INTO temp.hoardor_plex VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)");
    if (!insert) return std::unexpected(Error{"Plex: " + insert.error().message});
    if (auto r = plex->exec("BEGIN"); !r) return std::unexpected(Error{"Plex: " + r.error().message});
    while (true) {
        auto row = read->step();
        if (!row) return std::unexpected(Error{"reading Plex's database: " + row.error().message});
        if (!*row) break;
        const std::string path = read->column_text(0);
        insert->reset();
        insert->bind(1, key_of(path)).bind(2, normalized(path)).bind(3, read->column_int64(1));
        for (int c = 2; c < 8; ++c) insert->bind(c + 2, read->column_text(c));
        if (auto r = insert->run(); !r) return std::unexpected(Error{"Plex: " + r.error().message});
    }
    if (auto r = plex->exec("COMMIT; CREATE INDEX temp.hoardor_plex_key ON hoardor_plex(key);"); !r) {
        return std::unexpected(Error{"Plex: " + r.error().message});
    }
    auto lookup = plex->prepare("SELECT path, kind, hash, thumb, season_hash, season_thumb, show_hash, show_thumb "
                                "FROM temp.hoardor_plex WHERE key = ?");
    if (!lookup) return std::unexpected(Error{"Plex: " + lookup.error().message});
    auto state = std::make_unique<State>(State{std::move(*plex), plex_folder, std::move(*lookup)});
    return PlexPosters(std::move(state));
}

std::optional<fs::path> PlexPosters::poster(const fs::path& video_file) {
    const std::string path = file::detail::to_utf8(video_file).text;
    db::Statement& st = state_->lookup;
    st.reset();
    st.bind(1, key_of(path));
    struct Hit {
        std::string path, hash, thumb, season_hash, season_thumb, show_hash, show_thumb;
        int kind = 0;
    };
    std::vector<Hit> hits;
    while (true) {
        auto row = st.step();
        if (!row || !*row) break;
        hits.push_back({st.column_text(0), st.column_text(2), st.column_text(3), st.column_text(4), st.column_text(5),
                        st.column_text(6), st.column_text(7), static_cast<int>(st.column_int64(1))});
    }
    st.reset();
    if (hits.empty()) return std::nullopt;
    const Hit* hit = &hits.front();
    if (hits.size() > 1) {
        // The same folder and name more than once ("Season 1/01.mkv" in two shows, or one movie
        // on two drives): the path sharing the longest tail with ours; a tie is nobody's.
        const std::string mine = normalized(path);
        const auto shared_tail = [&](const std::string& other) {
            std::size_t n = 0;
            while (n < mine.size() && n < other.size() && mine[mine.size() - 1 - n] == other[other.size() - 1 - n]) ++n;
            return n;
        };
        std::size_t best = 0;
        bool tie = false;
        for (const Hit& h : hits) {
            const std::size_t n = shared_tail(h.path);
            if (n > best) best = n, hit = &h, tie = false;
            else if (n == best) tie = true;
        }
        if (tie) return std::nullopt;
    }
    if (hit->kind == plex_movie) return state_->image(hit->thumb, hit->hash, false);
    if (hit->kind == plex_episode) {
        // The grid shows shows: the show's poster, else the season's.
        if (auto show = state_->image(hit->show_thumb, hit->show_hash, true)) return show;
        return state_->image(hit->season_thumb, hit->season_hash, true);
    }
    return std::nullopt;
}

PlexPosters::PlexPosters(std::unique_ptr<State> state) : state_(std::move(state)) {}
PlexPosters::PlexPosters(PlexPosters&&) noexcept = default;
PlexPosters& PlexPosters::operator=(PlexPosters&&) noexcept = default;
PlexPosters::~PlexPosters() = default;

}
