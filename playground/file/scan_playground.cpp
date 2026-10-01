// Scans a directory and prints progress, counts by kind, errors, and timing.
// Usage: hoardor_scan <directory>
// For peak memory on Linux: /usr/bin/time -v ./hoardor_scan <directory>
// For a cold-cache run on Linux: sync; echo 3 | sudo tee /proc/sys/vm/drop_caches

#include <hoardor/file/scanner.hpp>

#include <chrono>
#include <iostream>
#include <map>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: hoardor_scan <directory>\n";
        return 2;
    }
    const auto start = std::chrono::steady_clock::now();
    auto scanner = hoardor::file::Scanner::open(argv[1], hoardor::file::Settings::defaults());
    if (!scanner) {
        std::cerr << "cannot open: " << scanner.error().message() << '\n';
        return 1;
    }

    std::map<hoardor::file::FileKind, std::uint64_t> by_kind;
    std::uint64_t bytes = 0;
    int shown_errors = 0;
    while (auto result = scanner->next()) {
        if (*result) {
            ++by_kind[(*result)->kind];
            bytes += (*result)->size;
            if (scanner->progress().files_emitted % 5000 == 0) {
                std::cerr << "  " << scanner->progress().files_emitted << " files...\n";
            }
        } else if (shown_errors++ < 20) {
            const auto& e = result->error();
            std::cerr << "  error: " << (e.relative_path.empty() ? "<root lost>" : e.relative_path) << ": "
                      << e.code.message() << (e.is_directory ? " (directory)" : "") << '\n';
        }
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    const auto progress = scanner->progress();
    std::cout << "directories: " << progress.directories_visited << '\n'
              << "files:       " << progress.files_emitted << " (" << bytes / (1024 * 1024) << " MiB)\n"
              << "errors:      " << progress.errors << '\n';
    for (const auto& [kind, count] : by_kind) std::cout << "  " << hoardor::file::to_string(kind) << ": " << count << '\n';
    std::cout << "time:        " << seconds << " s\n";
    return 0;
}
