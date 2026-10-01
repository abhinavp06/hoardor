#include <hoardor/file/scanner.hpp>

#include "support/temp_dir.hpp"

#include <benchmark/benchmark.h>

namespace {

// 50k empty files in 500 folders of 100 (ARCHITECTURE §7 target: < 1 s warm).
// Empty files cost the same as real ones: the scanner only stats them.
const hoardor::test::TempDir& library() {
    static const hoardor::test::TempDir dir;
    static const bool built = [] {
        for (int folder = 0; folder < 500; ++folder) {
            const auto album = std::filesystem::path("Artist " + std::to_string(folder / 10)) /
                               ("Album " + std::to_string(folder));
            for (int track = 0; track < 100; ++track) {
                dir.write(album / ("Track " + std::to_string(track) + ".flac"));
            }
        }
        return true;
    }();
    (void)built;
    return dir;
}

void BM_ScanFiftyThousandFiles(benchmark::State& state) {
    const auto& dir = library();
    const auto settings = hoardor::file::Settings::defaults();
    std::uint64_t files = 0;
    for (auto _ : state) {
        auto scanner = hoardor::file::Scanner::open(dir.path(), settings);
        files = 0;
        while (auto result = scanner->next()) files += result->has_value();
        benchmark::DoNotOptimize(files);
    }
    state.counters["files"] = static_cast<double>(files);
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(files));
}
BENCHMARK(BM_ScanFiftyThousandFiles)->Unit(benchmark::kMillisecond)->MinTime(2.0);

}
