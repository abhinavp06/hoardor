#pragma once

// Where a video's descriptive fields come from, besides its own tags: .nfo sidecar files
// (the Kodi/Jellyfin format) and, as the last resort, its file and folder names
// (features/media_listing.md §4.3). Internal; tested directly.

#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hoardor::video::detail {

// What an .nfo file (or a name) says. Empty / 0 = not said.
struct Described {
    // Extra: a featurette, deleted scene, trailer, … (it sits in an extras folder).
    // Sample: a release's sample clip, never listed.
    enum class Kind { Unknown, Movie, Episode, Show, Extra, Sample } kind = Kind::Unknown;
    std::string title;       // the movie's or the episode's title (a show's for Kind::Show)
    std::string show;        // an episode's show; an extra's show or movie (a guess from its folder)
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

// From the file name and the folders between `root` and the file (every folder up, when
// `root` is empty), nearest first (features/shows_and_extras.md):
// - in a "Sample" folder, or named "sample" -> a sample
// - below an extras folder ("Featurettes", "Extras", "Deleted Scenes", …, not the top
//   folder) -> an extra, titled "<subfolder> - <name>", its show guessed from the folder above
// - "Show S01E02 Title", "show.1x02", "Show S01 - E02", "Show E02", "Episode 2 Title",
//   "[Group] Show - 02", "Season 1/03 - Title" -> an episode (season 1 when nothing says)
// - "Title (2014)", "Title.2014.2160p.BluRay", "Title (2014 BluRay)" -> a movie with a year;
//   a name without a year takes its folder's ("Title (2014)/junk.mkv")
// - anything else -> a movie titled by the cleaned-up name
// An episode's show is the name before its number, else its show folder's, cleaned of
// release words ("Breaking Bad (2008) Season 1-5 (1080p BluRay …)" -> "Breaking Bad").
Described from_name(const std::filesystem::path& file, const std::filesystem::path& root = {});

// A show's (or a movie's) name from a folder name or a file-name prefix: [group] tags,
// release words (1080p, BluRay, x265, …), season words ("Season 1-5", "S01"), "Complete
// Series", and everything after them removed; a year in brackets is returned in `year`.
// Empty when nothing is left ("Season 1", "Specials").
std::string show_name(std::string_view text, int* year = nullptr);

// "Featurettes", "Extras", "1b. Extras", "Deleted Scenes", "Behind the Scenes", "Trailers", …
bool is_extras_folder(std::string_view name);

// The index of the outermost extras folder in a root-relative path's folders (top first), if
// any. The top folder never counts (a show called "Extras").
std::optional<std::size_t> extras_folder(std::span<const std::string> folders_top_first);

// "Season 1", "S01", "Series 2", "Staffel 3" -> the number; "Specials" -> 0; also with more
// text around it ("Season 1 (1999-2000)", "Show (2018) S01 (1080p …)"), but not a range
// ("Season 1-6", "S01-S06": a whole show's folder); else -1.
int season_of_folder(const std::string& name);

// Dots and underscores to spaces, collapsed, trimmed, and trailing " -" removed.
std::string clean_name(std::string text);

}
