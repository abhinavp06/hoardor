#include "video/sources.hpp"

#include "core/text.hpp"
#include "file/text.hpp"

#include <pugixml.hpp>

#include <algorithm>
#include <charconv>
#include <cstring>
#include <fstream>
#include <regex>
#include <sstream>

namespace hoardor::video::detail {

namespace {

int to_int(const std::string& text) {
    int value = 0;
    const std::string t = core::trim(text);
    const auto [ptr, ec] = std::from_chars(t.data(), t.data() + t.size(), value);
    return ec == std::errc{} ? value : 0;
}

int year_of(const std::string& text) {
    const int y = to_int(text.substr(0, 4));
    return (y >= 1800 && y <= 2999) ? y : 0;
}

std::vector<std::string> all_text(const pugi::xml_node& node, const char* name) {
    std::vector<std::string> out;
    for (const pugi::xml_node& child : node.children(name)) {
        // Some files put several values in one element, separated by " / ".
        for (auto& part : core::split_values(child.text().get(), "/")) {
            if (std::find(out.begin(), out.end(), part) == out.end()) out.push_back(std::move(part));
        }
    }
    return out;
}

std::string text_of(const pugi::xml_node& node, const char* name) { return core::trim(node.child(name).text().get()); }

}

std::optional<Described> parse_nfo(std::string text) {
    // Kodi allows a URL line after the XML: cut everything after the root's closing tag.
    for (const char* close : {"</movie>", "</episodedetails>", "</tvshow>"}) {
        if (const auto at = text.rfind(close); at != std::string::npos) {
            text.resize(at + std::strlen(close));
            break;
        }
    }
    pugi::xml_document doc;
    if (!doc.load_buffer(text.data(), text.size())) return std::nullopt;

    Described d;
    pugi::xml_node root;
    if ((root = doc.child("movie"))) d.kind = Described::Kind::Movie;
    else if ((root = doc.child("episodedetails"))) d.kind = Described::Kind::Episode;
    else if ((root = doc.child("tvshow"))) d.kind = Described::Kind::Show;
    else return std::nullopt;

    d.title = text_of(root, "title");
    d.plot = text_of(root, "plot");
    d.genres = all_text(root, "genre");
    d.directors = all_text(root, "director");
    d.writers = all_text(root, "credits");
    d.runtime_minutes = to_int(text_of(root, "runtime"));
    d.date = text_of(root, d.kind == Described::Kind::Episode ? "aired" : "premiered");
    d.year = year_of(text_of(root, "year"));
    if (d.year == 0) d.year = year_of(d.date);
    if (d.kind == Described::Kind::Episode) {
        d.show = text_of(root, "showtitle");
        const std::string season = text_of(root, "season");
        if (!season.empty()) d.season = to_int(season);
        d.episode = to_int(text_of(root, "episode"));
    }
    return d;
}

std::optional<Described> read_nfo(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return parse_nfo(buffer.str());
}

std::string clean_name(std::string text) {
    const auto digit = [](char c) { return c >= '0' && c <= '9'; };
    for (std::size_t i = 0; i < text.size(); ++i) {
        // A dot between digits stays ("Evangelion 3.33", "5.1").
        const bool decimal = text[i] == '.' && i > 0 && i + 1 < text.size() && digit(text[i - 1]) && digit(text[i + 1]);
        if ((text[i] == '.' && !decimal) || text[i] == '_') text[i] = ' ';
    }
    std::string out;
    for (char c : text) {
        if (c == ' ' && (out.empty() || out.back() == ' ')) continue;
        out.push_back(c);
    }
    out = core::trim(out);
    while (!out.empty() && (out.back() == '-' || out.back() == ' ' || out.back() == '(' || out.back() == '[')) out.pop_back();
    while (!out.empty() && (out.front() == '-' || out.front() == ' ')) out.erase(out.begin());
    return out;
}

namespace {

using std::regex_constants::icase;

// The folders between `root` and the file, nearest first.
std::vector<std::string> folders_of(const std::filesystem::path& file, const std::filesystem::path& root) {
    std::filesystem::path stop = root.lexically_normal();
    if (!stop.empty() && stop.filename().empty()) stop = stop.parent_path();  // "D:/Shows/"
    std::vector<std::string> out;
    for (std::filesystem::path p = file.parent_path(); !p.empty() && p.has_relative_path(); p = p.parent_path()) {
        if (!stop.empty() && p.lexically_normal() == stop) break;
        if (const std::string name = file::detail::to_utf8(p.filename()).text; !name.empty()) out.push_back(name);
        if (p == p.parent_path()) break;
    }
    return out;
}

// "[Anime Time] Show [BD]" -> " Show ": release groups and tags in square brackets.
std::string without_bracket_tags(std::string_view text) {
    static const std::regex tags(R"(\[[^\]]*\])");
    return std::regex_replace(std::string(text), tags, " ");
}

// Release words and what follows them: quality, source, codec.
const std::regex& release_words() {
    static const std::regex r(
        R"((?:^|[ ._(\[-])(?:\d{3,4}p|4k|uhd|blu-?ray|bd-?rip|br-?rip|remux|web-?dl|web-?rip|hdtv|dvd-?rip|x26[45]|h[ ._]?26[45]|hevc|10[ ._-]?bits?)(?=$|[^a-z0-9]))",
        icase);
    return r;
}

// An episode's or a movie's title without the release words after it, and without a trailing
// checksum ("Pilot (1080p BluRay x265 Silence)" -> "Pilot", "The Driver [113C492A]" -> "The Driver").
std::string title_of(std::string text) {
    std::smatch m;
    if (std::regex_search(text, m, release_words())) {
        text.resize(static_cast<std::size_t>(m.position(0)));
        // A bracket the cut left open goes too: "Title (2002 - 480p)" -> "Title".
        const auto open = text.find_last_of("([");
        if (open != std::string::npos && text.find_first_of(")]", open) == std::string::npos) text.resize(open);
    }
    static const std::regex checksum(R"(\s*\[[0-9a-f]{8}\]\s*$)", icase);
    text = std::regex_replace(text, checksum, "");
    return clean_name(text);
}

// What a file name says about its episode number, if anything.
struct EpisodeMatch {
    int season = -1, episode = 0;
    std::string before, after;  // the name around the number
};

std::optional<EpisodeMatch> episode_in(const std::string& stem) {
    static const std::regex sxxexx(R"((?:^|[^a-z0-9])s(\d{1,4})[ ._-]*e(\d{1,4}))", icase);
    static const std::regex nxnn(R"((?:^|[^0-9])(\d{1,2})x(\d{2,3})(?:[^0-9]|$))", icase);
    static const std::regex word(R"((?:^|[^a-z0-9])(?:episode|ep)[ ._-]*(\d{1,4})(?!\d))", icase);
    static const std::regex e_only(R"((?:^|[ ._-])e(\d{2,4})(?=$|[ ._-]))", icase);
    static const std::regex bracket_year(R"([\(\[](?:19|20)\d{2})");
    std::smatch m;
    const auto around = [&](int season, int episode) {
        EpisodeMatch e;
        e.season = season;
        e.episode = episode;
        e.before = stem.substr(0, static_cast<std::size_t>(m.position(0)));
        e.after = stem.substr(static_cast<std::size_t>(m.position(0) + m.length(0)));
        return e;
    };
    if (std::regex_search(stem, m, sxxexx) || std::regex_search(stem, m, nxnn)) {
        EpisodeMatch e = around(std::stoi(m[1].str()), std::stoi(m[2].str()));
        // A double episode's second number isn't part of the title ("S01E01-E02 - Title").
        static const std::regex more(R"(^[ ._-]*(?:e\d{1,4}[ ._-]*)+)", icase);
        e.after = std::regex_replace(e.after, more, " ");
        return e;
    }
    // "Episode 4" in a name with a year is a movie ("Star Wars Episode 4 (1977)").
    if (std::regex_search(stem, m, word) && !std::regex_search(stem, bracket_year)) return around(-1, std::stoi(m[1].str()));
    if (std::regex_search(stem, m, e_only)) return around(-1, std::stoi(m[1].str()));
    // "[Group] Show - 01", "Show - 1001 [1080p]", "Show - 05v2": a number after " - " at the end
    // (only bracketed tags may follow). Years don't count ("Blade Runner - 2049").
    static const std::regex dash(R"(^(.*?\S)\s+-\s+(\d{1,4})(?:v\d)?\s*((?:[\[(][^\])]*[\])]\s*)*)$)", icase);
    if (std::regex_match(stem, m, dash)) {
        const int n = std::stoi(m[2].str());
        if (!(m[2].length() == 4 && n >= 1900 && n <= 2099)) {
            EpisodeMatch e{-1, n, m[1].str(), {}};
            return e;
        }
    }
    return std::nullopt;
}

bool is_sample(const std::string& stem, const std::vector<std::string>& folders) {
    static const std::regex name(R"((?:^|[ ._-])sample$|^sample(?:$|[ ._-]))", icase);
    static const std::regex folder(R"(^samples?$)", icase);
    if (std::regex_search(stem, name)) return true;
    return std::any_of(folders.begin(), folders.end(), [](const std::string& f) { return std::regex_match(f, folder); });
}

// A show's name from the folders, nearest first: the first one that isn't only a season.
std::string show_from_folders(const std::vector<std::string>& folders, std::size_t from, int* year) {
    for (std::size_t i = from; i < folders.size(); ++i) {
        if (std::string name = show_name(folders[i], year); !name.empty()) return name;
    }
    return {};
}

}

int season_of_folder(const std::string& name) {
    static const std::regex season(R"(^\s*(?:season|series|staffel|saison|temporada|s)\s*[._ -]?\s*(\d{1,3})\s*$)", icase);
    static const std::regex specials(R"(^\s*(?:specials?|extras?)\s*$)", icase);
    std::smatch m;
    if (std::regex_match(name, m, season)) return to_int(m[1].str());
    if (std::regex_match(name, specials)) return 0;
    // With more text: "Season 1 (1999-2000)", "Succession (2018) S01", but not "Season 1-6" or
    // "S01-S06", which name a whole show's folder.
    static const std::regex range_after(R"(^[ ._]*(?:-|–|~|to)[ ._]*(?:s|season)?[ ._]*\d)", icase);
    static const std::regex word(R"((?:^|[^a-z0-9])(?:season|series|staffel|saison|temporada)[ ._-]*(\d{1,4})(?!\d))", icase);
    static const std::regex short_form(R"((?:^|[^a-z0-9])s(\d{1,2})(?!\d))", icase);
    for (const std::regex* r : {&word, &short_form}) {
        if (std::regex_search(name, m, *r) && !std::regex_search(m.suffix().str(), range_after)) return to_int(m[1].str());
    }
    static const std::regex specials_with_more(R"(^\s*specials?(?:$|[^a-z]))", icase);
    if (std::regex_search(name, specials_with_more)) return 0;
    return -1;
}

std::string show_name(std::string_view text, int* year) {
    std::string t = without_bracket_tags(text);
    static const std::regex bracketed_year(R"([\(\[]\s*((?:19|20)\d{2}))");
    static const std::regex dotted_year(R"([ ._]((?:19|20)\d{2})(?=$|[ ._]))");
    static const std::regex season_words(
        R"((?:^|[ ._-])(?:s\d{1,4}(?:[ ._-]*e\d{1,4})?|(?:season|series)[ ._-]*\d+|specials?)(?=$|[^a-z0-9]))", icase);
    std::size_t cut = t.size();
    int found_year = 0;
    std::smatch m;
    const auto earlier = [&](const std::regex& r, bool is_year) {
        if (std::regex_search(t, m, r) && static_cast<std::size_t>(m.position(0)) < cut) {
            cut = static_cast<std::size_t>(m.position(0));
            if (is_year) found_year = to_int(m[1].str());
        }
    };
    earlier(bracketed_year, true);
    earlier(dotted_year, true);
    earlier(season_words, false);
    earlier(release_words(), false);
    static const std::regex complete(R"((?:^|[ ._-])(?:the[ ._-]+)?complete(?=$|[^a-z0-9]))", icase);
    earlier(complete, false);
    if (year && found_year) *year = found_year;
    t.resize(cut);
    std::string out = clean_name(t);
    while (!out.empty() && (out.back() == ',' || out.back() == '-' || out.back() == ' ')) out.pop_back();
    return out;
}

bool is_extras_folder(std::string_view name) {
    static const std::regex ordinal(R"(^\s*\d{1,2}[a-z]?[.)]\s+)", icase);
    static const std::regex words(
        R"(^(?:featurettes?|extras?|bonus(?:[ ._-]+(?:features|material|content))?|special[ ._-]+features|behind[ ._-]+the[ ._-]+scenes|deleted[ ._-]+scenes|interviews?|trailers?|teasers?|making[ ._-]+of)(?:$|[^a-z]))",
        icase);
    static const std::regex exact(R"(^(?:other|others|scenes|shorts)$)", icase);
    const std::string n = std::regex_replace(core::trim(name), ordinal, "");
    return std::regex_search(n, words) || std::regex_match(n, exact);
}

std::optional<std::size_t> extras_folder(std::span<const std::string> folders_top_first) {
    for (std::size_t i = 1; i < folders_top_first.size(); ++i) {
        if (is_extras_folder(folders_top_first[i])) return i;
    }
    return std::nullopt;
}

Described from_name(const std::filesystem::path& file, const std::filesystem::path& root) {
    const std::string stem = file::detail::to_utf8(file.stem()).text;
    const std::vector<std::string> folders = folders_of(file, root);  // nearest first
    Described d;

    if (is_sample(stem, folders)) {
        d.kind = Described::Kind::Sample;
        d.title = clean_name(stem);
        return d;
    }

    // Extras: below the outermost extras folder (never the top folder).
    const std::vector<std::string> top_first(folders.rbegin(), folders.rend());
    if (const auto at = extras_folder(top_first)) {
        const std::size_t extras = folders.size() - 1 - *at;  // its index, nearest first
        d.kind = Described::Kind::Extra;
        d.title = title_of(without_bracket_tags(stem));
        if (d.title.empty()) d.title = clean_name(stem);
        // The subfolder that says what it is ("Deleted Scenes - Bingo"), seasons aside.
        for (std::size_t i = 0; i < extras; ++i) {
            if (season_of_folder(folders[i]) >= 0) {
                if (d.season < 0) d.season = season_of_folder(folders[i]);
                continue;
            }
            // Not a folder that only says "extras" again ("Featurettes 5.1").
            static const std::regex generic(R"(^\s*(?:\d{1,2}[a-z]?[.)]\s+)?(?:featurettes?|extras?|bonus)(?:$|[^a-z]))", icase);
            if (i == 0 && !std::regex_search(folders[i], generic)) d.title = clean_name(folders[i]) + " - " + d.title;
        }
        d.show = show_from_folders(folders, extras + 1, &d.year);
        return d;
    }

    const int folder_season = folders.empty() ? -1 : season_of_folder(folders[0]);
    if (auto e = episode_in(stem)) {
        d.kind = Described::Kind::Episode;
        d.season = e->season >= 0 ? e->season : (folder_season >= 0 ? folder_season : 1);
        d.episode = e->episode;
        d.title = title_of(e->after);
        int year = 0;
        const std::string from_folders = show_from_folders(folders, 0, &year);
        d.show = show_name(e->before);
        // The folder's spelling of the same name when the file's is all lowercase
        // ("adolescence.s01e01" in "Adolescence S01").
        const bool lowercase = std::none_of(d.show.begin(), d.show.end(), [](char c) { return c >= 'A' && c <= 'Z'; });
        if (d.show.empty() || (lowercase && core::sort_key(d.show) == core::sort_key(from_folders))) d.show = from_folders;
        return d;
    }
    if (folder_season >= 0) {
        // "Season 1/03 - Title.mkv"
        static const std::regex leading(R"(^\s*(?:e|ep|episode)?\s*(\d{1,3})(?:[ ._-]+(.*))?$)", icase);
        std::smatch m;
        if (std::regex_match(stem, m, leading)) {
            d.kind = Described::Kind::Episode;
            d.season = folder_season;
            d.episode = to_int(m[1].str());
            d.title = title_of(m[2].str());
            d.show = show_from_folders(folders, 1, nullptr);
            return d;
        }
    }

    d.kind = Described::Kind::Movie;
    // "(2014)", "[1968]", "(1989 BluRay - 1080p)"
    static const std::regex year_in_brackets(R"(^(.*?)[ ._]*[\(\[]((?:19|20)\d{2})(?:[^\)\]\d][^\)\]]*)?[\)\]])");
    // Greedy: the last year-like number is the year ("Blade.Runner.2049.2017.2160p" -> 2017).
    static const std::regex year_dotted(R"(^(.+)[ ._]((?:19|20)\d{2})(?:[ ._]|$))");
    const auto with_year = [&](const std::string& text) {
        std::smatch m;
        if ((std::regex_search(text, m, year_in_brackets) || std::regex_search(text, m, year_dotted)) && year_of(m[2].str())) {
            d.title = clean_name(without_bracket_tags(m[1].str()));
            d.year = year_of(m[2].str());
            return !d.title.empty();
        }
        return false;
    };
    if (!with_year(stem)) {
        // A name that says nothing about its year takes its folder's ("Title (2014)/junk.mkv").
        if (folders.empty() || season_of_folder(folders[0]) >= 0 || !with_year(folders[0])) {
            d.year = 0;
            d.title = title_of(without_bracket_tags(stem));
        }
    }
    if (d.title.empty()) d.title = clean_name(stem);
    return d;
}

}
