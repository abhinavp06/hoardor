#include <hoardor/file/library.hpp>

#include "support/temp_dir.hpp"

#include <benchmark/benchmark.h>

namespace {

namespace fs = std::filesystem;

// 50k empty files in 500 folders of 100, shared by both benchmarks.
const hoardor::test::TempDir& tree() {
    static const hoardor::test::TempDir dir;
    static const bool built = [] {
        for (int folder = 0; folder < 500; ++folder) {
            const auto album = fs::path("Music") / ("Artist " + std::to_string(folder / 10)) / ("Album " + std::to_string(folder));
            for (int track = 0; track < 100; ++track) dir.write(album / ("Track " + std::to_string(track) + ".flac"));
        }
        return true;
    }();
    (void)built;
    return dir;
}

// A database file with one root over the tree. Members are declared in the order they
// must be built and destroyed: the Library points at the Database.
struct Synced {
    hoardor::test::TempDir db_dir;
    std::optional<hoardor::db::Database> db;
    std::optional<hoardor::file::Library> library;
    hoardor::file::RootId root = 0;
};

std::unique_ptr<Synced> fresh_library() {
    auto s = std::make_unique<Synced>();
    s->db.emplace(*hoardor::db::Database::open(s->db_dir.path() / "library.db"));
    s->library.emplace(*hoardor::file::Library::open(*s->db, [] { return std::vector<fs::path>{}; }));
    // The tree was just written: without this its files would be "still being copied".
    auto settings = *s->library->load_settings();
    settings.settle_window_seconds = 0;
    (void)s->library->save_settings(settings);
    const auto music = s->library->categories()->front().id;
    s->root = s->library->add_root(music, tree().path() / "Music")->id;
    return s;
}

// The first sync of a library: every file is an insert.
void BM_FirstSyncFiftyThousandFiles(benchmark::State& state) {
    for (auto _ : state) {
        state.PauseTiming();
        auto lib = fresh_library();
        state.ResumeTiming();
        const auto report = lib->library->sync_root(lib->root);
        auto added = report.added;
        benchmark::DoNotOptimize(added);
        state.counters["added"] = static_cast<double>(report.added);
        state.PauseTiming();
        lib.reset();
        state.ResumeTiming();
    }
}
BENCHMARK(BM_FirstSyncFiftyThousandFiles)->Unit(benchmark::kMillisecond)->Iterations(3);

// The everyday case: press Sync, nothing (or almost nothing) changed.
void BM_IncrementalSyncFiftyThousandFiles(benchmark::State& state) {
    auto lib = fresh_library();
    lib->library->sync_root(lib->root);
    for (auto _ : state) {
        const auto report = lib->library->sync_root(lib->root);
        auto unchanged = report.unchanged;
        benchmark::DoNotOptimize(unchanged);
        state.counters["unchanged"] = static_cast<double>(report.unchanged);
    }
}
BENCHMARK(BM_IncrementalSyncFiftyThousandFiles)->Unit(benchmark::kMillisecond)->MinTime(2.0);

}
