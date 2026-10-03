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
    for (char& c : text) {
        if (c == '.' || c == '_') c = ' ';
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

int season_of_folder(const std::string& name) {
    static const std::regex season(R"(^\s*(?:season|series|staffel|saison|temporada|s)\s*[._ -]?\s*(\d{1,3})\s*$)", std::regex::icase);
    static const std::regex specials(R"(^\s*(?:specials?|extras?)\s*$)", std::regex::icase);
    std::smatch m;
    if (std::regex_match(name, m, season)) return to_int(m[1].str());
    if (std::regex_match(name, specials)) return 0;
    return -1;
}

Described from_name(const std::filesystem::path& file) {
    const std::string stem = file::detail::to_utf8(file.stem()).text;
    const std::string folder = file::detail::to_utf8(file.parent_path().filename()).text;
    const std::string grandparent = file::detail::to_utf8(file.parent_path().parent_path().filename()).text;
    Described d;

    static const std::regex sxxexx(R"((?:^|[^a-z0-9])s(\d{1,2})[ ._-]?e(\d{1,3}))", std::regex::icase);
    static const std::regex nxnn(R"((?:^|[^0-9])(\d{1,2})x(\d{2,3})(?:[^0-9]|$))", std::regex::icase);
    std::smatch m;
    const int folder_season = season_of_folder(folder);
    if (std::regex_search(stem, m, sxxexx) || std::regex_search(stem, m, nxnn)) {
        d.kind = Described::Kind::Episode;
        d.season = to_int(m[1].str());
        d.episode = to_int(m[2].str());
        const std::string before = clean_name(stem.substr(0, static_cast<std::size_t>(m.position(0))));
        const std::string after = clean_name(stem.substr(static_cast<std::size_t>(m.position(0) + m.length(0))));
        d.title = after;
        if (folder_season >= 0 && !grandparent.empty()) d.show = grandparent;
        else if (!before.empty()) d.show = before;
        else d.show = folder;
        return d;
    }
    if (folder_season >= 0) {
        // "Season 1/03 - Title.mkv"
        static const std::regex leading(R"(^\s*(?:e|ep|episode)?\s*(\d{1,3})(?:[ ._-]+(.*))?$)", std::regex::icase);
        if (std::regex_match(stem, m, leading)) {
            d.kind = Described::Kind::Episode;
            d.season = folder_season;
            d.episode = to_int(m[1].str());
            d.title = clean_name(m[2].str());
            d.show = grandparent;
            return d;
        }
    }

    d.kind = Described::Kind::Movie;
    static const std::regex year_in_brackets(R"(^(.*?)[ ._]*[\(\[](\d{4})[\)\]])");
    // Greedy: the last year-like number is the year ("Blade.Runner.2049.2017.2160p" -> 2017).
    static const std::regex year_dotted(R"(^(.+)[ ._]((?:19|20)\d{2})(?:[ ._]|$))");
    if ((std::regex_search(stem, m, year_in_brackets) || std::regex_search(stem, m, year_dotted)) && year_of(m[2].str())) {
        d.title = clean_name(m[1].str());
        d.year = year_of(m[2].str());
    }
    if (d.title.empty()) d.title = clean_name(stem);
    return d;
}

}
