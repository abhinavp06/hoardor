#pragma once

// Where a video's descriptive fields come from, besides its own tags: .nfo sidecar files
// (the Kodi/Jellyfin format) and, as the last resort, its file and folder names
// (features/media_listing.md §4.3). Internal; tested directly.

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace hoardor::video::detail {

// What an .nfo file (or a name) says. Empty / 0 = not said.
struct Described {
    enum class Kind { Unknown, Movie, Episode, Show } kind = Kind::Unknown;
    std::string title;       // the movie's or the episode's title (a show's for Kind::Show)
    std::string show;        // an episode's show
    int season = -1;         // -1: unknown (0 is "Specials")
    int episode = 0;
    int year = 0;
    std::string date;        // premiered / aired
    std::string plot;
    std::vector<std::string> genres, directors, writers;
    int runtime_minutes = 0;
};

// Parses an .nfo file: <movie>, <episodedetails>, or <tvshow>. nullopt when it isn't one
// (a link-only .nfo, broken XML). Kodi files may have a URL after the XML; that's ignored.
std::optional<Described> read_nfo(const std::filesystem::path& file);
std::optional<Described> parse_nfo(std::string text);

// From the file name and its folders, relative to nothing in particular (the last two
// folder names are used):
// - "Show S01E02 Title.mkv", "show.1x02.mkv", "Season 1/03 - Title.mkv" -> an episode
// - "Title (2014).mkv", "Title.2014.2160p.BluRay.mkv" -> a movie with a year
// - anything else -> a movie titled by the cleaned-up name
Described from_name(const std::filesystem::path& file);

// "Season 1", "S01", "Series 2", "Staffel 3" -> the number; "Specials" -> 0; else -1.
int season_of_folder(const std::string& name);

// Dots and underscores to spaces, collapsed, trimmed, and trailing " -" removed.
std::string clean_name(std::string text);

}
