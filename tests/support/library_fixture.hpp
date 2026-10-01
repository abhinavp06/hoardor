#pragma once

#include <hoardor/file/library.hpp>

#include "support/temp_dir.hpp"

#include <gtest/gtest.h>

#include <memory>

namespace hoardor::test {

// An in-memory database with a file::Library whose mount points are whatever the test sets.
class LibraryTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto database = db::Database::open_in_memory();
        ASSERT_TRUE(database.has_value());
        db = std::make_unique<db::Database>(std::move(*database));
        auto lib = file::Library::open(*db, [this] { return mounts; });
        ASSERT_TRUE(lib.has_value()) << lib.error().message;
        library = std::make_unique<file::Library>(std::move(*lib));
    }

    file::CategoryId category(std::string_view name) {
        // Keep the vector alive: iterating `categories().value()` directly would dangle
        // (lifetime extension in range-for over a temporary's member is C++23 P2718, not in GCC 13).
        const auto all = library->categories().value();
        for (const auto& c : all) {
            if (c.name == name) return c.id;
        }
        ADD_FAILURE() << "no category " << name;
        return 0;
    }

    std::vector<file::Entry> all_entries(file::RootId root) {
        std::vector<file::Entry> out;
        file::EntryId after = 0;
        while (true) {
            auto page = library->entries(root, after, 100).value();
            if (page.empty()) return out;
            after = page.back().id;
            out.insert(out.end(), page.begin(), page.end());
        }
    }

    void set_settings(const std::function<void(file::Settings&)>& edit) {
        auto s = library->load_settings().value();
        edit(s);
        ASSERT_TRUE(library->save_settings(s));
    }

    TempDir dir;
    std::vector<std::filesystem::path> mounts;  // what list_mount_points() returns in these tests
    std::unique_ptr<db::Database> db;
    std::unique_ptr<file::Library> library;
};

}
