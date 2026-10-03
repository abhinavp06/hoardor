// The audio engine's generic queries on a large library (features/media_listing.md §7):
// HOARDOR_BENCH_FILES tracks (default 50,000), 10 per album, 20 albums per artist.
// Rows are stored through audio::Library::store against synthetic file entries; no media files.

#include <hoardor/audio/audio.hpp>
#include <hoardor/file/library.hpp>

#include "support/bench_tree.hpp"

#include <benchmark/benchmark.h>

namespace {

namespace fs = std::filesystem;
using namespace hoardor;
using audio::Field;

struct Catalog {
    test::TempDir dir;
    std::optional<db::Database> db;
    std::optional<file::Library> files;
    std::optional<audio::Library> tracks;
    std::size_t count = 0;
};

Catalog& catalog() {
    static Catalog* c = [] {
        auto* c = new Catalog;
        c->count = static_cast<std::size_t>(bench::file_count());
        c->db.emplace(*db::Database::open(c->dir.path() / "library.db"));
        c->files.emplace(*file::Library::open(*c->db, [] { return std::vector<fs::path>{}; }));
        c->tracks.emplace(*audio::Library::open(*c->db));
        fs::create_directories(c->dir.path() / "m");
        const auto root = c->files->add_root(c->files->categories()->front().id, c->dir.path() / "m", "m", false)->id;
        auto tx = db::Transaction::begin(*c->db);
        auto insert = c->db->prepare("INSERT INTO file_entries (id, root_id, relative_path, path_key, size, mtime_ns, kind, "
                                     "seen_generation, changed_generation, added_ns) VALUES (?, ?, ?, ?, 1, 1, 1, 1, 1, ?)");
        for (std::size_t i = 1; i <= c->count; ++i) {
            const std::string path = "t" + std::to_string(i) + ".flac";
            insert->bind(1, static_cast<std::int64_t>(i)).bind(2, root).bind(3, std::string_view(path)).bind(4, std::string_view(path))
                .bind(5, static_cast<std::int64_t>(i));
            (void)insert->run();
            insert->reset();
            const std::size_t album = (i - 1) / 10, artist = album / 20;
            audio::TrackInfo t;
            t.title = "Track " + std::to_string(i);
            t.album = "Album " + std::to_string(album);
            t.album_artist = "Artist " + std::to_string(artist);
            t.artists = {t.album_artist};
            t.genres = {"Genre " + std::to_string(album % 40)};
            t.track = static_cast<int>((i - 1) % 10 + 1);
            t.disc = 1;
            t.year = 1950 + static_cast<int>(album % 70);
            t.duration_ms = 240'000;
            t.codec = "flac";
            (void)c->tracks->store(static_cast<std::int64_t>(i), 1, 1, t);
        }
        (void)tx->commit();
        return c;
    }();
    return *c;
}

const std::vector<Field> album{Field::AlbumArtist, Field::Album};

void BM_AlbumsFirstPageByName(benchmark::State& state) {
    auto& c = catalog();
    for (auto _ : state) benchmark::DoNotOptimize(c.tracks->groups(album, {}, audio::GroupOrder::Values, false, std::nullopt, 200));
}

void BM_AlbumsMiddlePageByName(benchmark::State& state) {
    auto& c = catalog();
    const auto half = c.tracks->groups(album, {}, audio::GroupOrder::Values, false, std::nullopt, c.count / 20)->next;
    for (auto _ : state) benchmark::DoNotOptimize(c.tracks->groups(album, {}, audio::GroupOrder::Values, false, half, 200));
}

void BM_AlbumsFirstPageNewestAdded(benchmark::State& state) {
    auto& c = catalog();
    for (auto _ : state) benchmark::DoNotOptimize(c.tracks->groups(album, {}, audio::GroupOrder::AddedLast, true, std::nullopt, 200));
}

void BM_AlbumsOfAGenre(benchmark::State& state) {
    auto& c = catalog();
    const audio::Filter genre{{{Field::Genre, audio::Value{"Genre 7"}}}};
    for (auto _ : state) benchmark::DoNotOptimize(c.tracks->groups(album, genre, audio::GroupOrder::Values, false, std::nullopt, 200));
}

void BM_TracksOfAnAlbum(benchmark::State& state) {
    auto& c = catalog();
    const audio::Filter one{{{Field::AlbumArtist, audio::Value{"Artist 3"}}, {Field::Album, audio::Value{"Album 61"}}}};
    const std::vector<audio::Order> order{{Field::Disc}, {Field::Track}};
    for (auto _ : state) benchmark::DoNotOptimize(c.tracks->tracks(one, order));
}

void BM_TracksMiddlePageByTitle(benchmark::State& state) {
    auto& c = catalog();
    const std::vector<audio::Order> order{{Field::Title}};
    const auto half = c.tracks->tracks({}, order, std::nullopt, c.count / 2)->next;
    for (auto _ : state) benchmark::DoNotOptimize(c.tracks->tracks({}, order, half, 200));
}

void BM_AlbumCount(benchmark::State& state) {
    auto& c = catalog();
    for (auto _ : state) benchmark::DoNotOptimize(c.tracks->group_count(album, {}));
}

void BM_TrackCount(benchmark::State& state) {
    auto& c = catalog();
    for (auto _ : state) benchmark::DoNotOptimize(c.tracks->count({}));
}

void BM_PendingCountWhenNothingIsPending(benchmark::State& state) {
    auto& c = catalog();
    for (auto _ : state) benchmark::DoNotOptimize(c.tracks->pending_count(std::nullopt));
}

}

BENCHMARK(BM_AlbumsFirstPageByName)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_AlbumsMiddlePageByName)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_AlbumsFirstPageNewestAdded)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_AlbumsOfAGenre)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_TracksOfAnAlbum)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_TracksMiddlePageByTitle)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_AlbumCount)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_TrackCount)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_PendingCountWhenNothingIsPending)->Unit(benchmark::kMillisecond);
