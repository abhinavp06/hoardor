#include <hoardor/file/scanner.hpp>

#include "support/bench_tree.hpp"

#include <benchmark/benchmark.h>

namespace {

// A full scan of the generated library (ARCHITECTURE §7: 50k files in < 1 s warm).
void BM_Scan(benchmark::State& state) {
    const auto root = hoardor::bench::library_tree().path() / "Music";
    const auto settings = hoardor::file::Settings::defaults();
    std::uint64_t files = 0;
    for (auto _ : state) {
        auto scanner = hoardor::file::Scanner::open(root, settings);
        files = 0;
        while (auto result = scanner->next()) files += result->has_value();
        benchmark::DoNotOptimize(files);
    }
    state.counters["files"] = static_cast<double>(files);
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(files));
}
BENCHMARK(BM_Scan)->Unit(benchmark::kMillisecond)->MinTime(2.0);

}
