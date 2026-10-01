// Syncs a real folder into a database and prints the report, for manual runs
// against an external drive (cold-cache timing, unplugging mid-sync, ...).
//
// Usage: hoardor_sync <database file> <category name> <folder>
//   - the folder is added as a root of the category on first use
//   - run it again to see an incremental sync
// Example: hoardor_sync ~/hoardor-test.db Music /media/me/HDD_A/Music

#include <hoardor/file/library.hpp>

#include <algorithm>
#include <chrono>
#include <iostream>

using namespace hoardor;

int main(int argc, char** argv) {
    if (argc != 4) {
        std::cerr << "usage: hoardor_sync <database file> <category name> <folder>\n";
        return 2;
    }
    auto database = db::Database::open(argv[1]);
    if (!database) {
        std::cerr << "cannot open database: " << database.error().message << '\n';
        return 1;
    }
    auto library = file::Library::open(*database);
    if (!library) {
        std::cerr << library.error().message << '\n';
        return 1;
    }

    const auto categories = library->categories().value();
    const auto category = std::find_if(categories.begin(), categories.end(), [&](const auto& c) { return c.name == argv[2]; });
    if (category == categories.end()) {
        std::cerr << "no category named " << argv[2] << '\n';
        return 1;
    }

    const auto folder = std::filesystem::weakly_canonical(argv[3]);
    std::optional<file::RootId> root_id;
    const auto roots = library->roots().value();  // a named vector: iterating .value() of a temporary would dangle
    for (const auto& r : roots) {
        if (std::filesystem::path(r.path) == folder) root_id = r.id;
    }
    if (!root_id) {
        auto added = library->add_root(category->id, folder);
        if (!added) {
            std::cerr << "cannot add folder: " << added.error().message << '\n';
            return 1;
        }
        root_id = added->id;
        std::cout << "added root " << added->name << " (marker: " << (added->use_marker ? "yes" : "no")
                  << ", case-sensitive: " << (added->case_sensitive ? "yes" : "no") << ")\n";
    }

    const auto start = std::chrono::steady_clock::now();
    const auto report = library->sync_root(*root_id, {}, [](const file::SyncProgress& p) {
        if (p.scan.files_emitted % 5000 == 0) std::cerr << "  " << p.scan.files_emitted << " files...\n";
    });
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    const char* outcomes[] = {"synced", "offline", "cancelled", "failed"};
    std::cout << "outcome:    " << outcomes[static_cast<int>(report.outcome)] << (report.message.empty() ? "" : " - " + report.message)
              << "\nadded:      " << report.added << "\nmodified:   " << report.modified << "\nremoved:    " << report.removed
              << "\nunchanged:  " << report.unchanged << "\nunsettled:  " << report.unsettled << "\nerrors:     " << report.errors
              << "\nheld:       " << report.held_removals << "\nrelocated:  " << (report.relocated ? "yes" : "no")
              << "\ntime:       " << seconds << " s\n";
    const auto errors = library->scan_errors(*root_id, 20).value();
    for (const auto& e : errors) {
        std::cout << "  error: " << (e.relative_path.empty() ? "<root>" : e.relative_path) << ": " << e.message << '\n';
    }
    return report.outcome == file::RootSyncOutcome::Synced ? 0 : 1;
}
