#include <hoardor/db/database.hpp>

#include "support/temp_dir.hpp"

#include <gtest/gtest.h>

#include <array>

using hoardor::db::Database;
using hoardor::db::Migration;
using hoardor::db::Transaction;

namespace {

Database memory() {
    auto db = Database::open_in_memory();
    EXPECT_TRUE(db.has_value()) << db.error().message;
    return std::move(*db);
}

std::int64_t count(Database& db, std::string_view table) {
    auto st = db.prepare("SELECT COUNT(*) FROM " + std::string(table));
    EXPECT_TRUE(st.has_value());
    EXPECT_TRUE(st->step().value());
    return st->column_int64(0);
}

}

TEST(Database, OpensInMemoryWithForeignKeysOn) {
    Database db = memory();
    auto st = db.prepare("PRAGMA foreign_keys");
    ASSERT_TRUE(st.has_value());
    ASSERT_TRUE(st->step().value());
    EXPECT_EQ(st->column_int64(0), 1);
}

TEST(Database, OpensFileInWalMode) {
    hoardor::test::TempDir dir;
    auto db = Database::open(dir.path() / "library.db");
    ASSERT_TRUE(db.has_value()) << db.error().message;
    auto st = db->prepare("PRAGMA journal_mode");
    ASSERT_TRUE(st->step().value());
    EXPECT_EQ(st->column_text(0), "wal");
}

TEST(Database, OpenFailsForImpossiblePath) {
    hoardor::test::TempDir dir;
    auto db = Database::open(dir.path() / "missing-folder" / "library.db");
    EXPECT_FALSE(db.has_value());
}

TEST(Database, ReportsSqlErrors) {
    Database db = memory();
    EXPECT_FALSE(db.exec("CREATE TABLEE nope").has_value());
    auto st = db.prepare("SELECT * FROM missing_table");
    ASSERT_FALSE(st.has_value());
    EXPECT_NE(st.error().message.find("missing_table"), std::string::npos);
}

TEST(Statement, BindsAndReadsEveryType) {
    Database db = memory();
    ASSERT_TRUE(db.exec("CREATE TABLE t (i INTEGER, d REAL, s TEXT, n TEXT)"));
    auto insert = db.prepare("INSERT INTO t VALUES (?, ?, ?, ?)");
    ASSERT_TRUE(insert.has_value());
    const std::string text("caf\xC3\xA9 \xF0\x9F\x8E\xB5 with\0nul", 22);
    insert->bind(1, std::int64_t{9'000'000'000'000'000'000}).bind(2, 2.5).bind(3, std::string_view(text)).bind_null(4);
    ASSERT_TRUE(insert->run());
    EXPECT_EQ(db.changes(), 1);
    EXPECT_EQ(db.last_insert_id(), 1);

    auto select = db.prepare("SELECT i, d, s, n FROM t");
    ASSERT_TRUE(select->step().value());
    EXPECT_EQ(select->column_int64(0), 9'000'000'000'000'000'000);
    EXPECT_DOUBLE_EQ(select->column_double(1), 2.5);
    EXPECT_EQ(select->column_text(2), text);
    EXPECT_TRUE(select->column_is_null(3));
    EXPECT_FALSE(select->step().value());
}

TEST(Statement, ResetAllowsReuse) {
    Database db = memory();
    ASSERT_TRUE(db.exec("CREATE TABLE t (v INTEGER)"));
    auto insert = db.prepare("INSERT INTO t VALUES (?)");
    for (int i = 0; i < 3; ++i) {
        insert->bind(1, i);
        ASSERT_TRUE(insert->run());
        insert->reset();
    }
    EXPECT_EQ(count(db, "t"), 3);
}

TEST(Statement, BindErrorIsReportedByStep) {
    Database db = memory();
    auto st = db.prepare("SELECT ?");
    st->bind(5, 1);  // out of range
    EXPECT_FALSE(st->step().has_value());
}

TEST(Transaction, CommitKeepsChanges) {
    Database db = memory();
    ASSERT_TRUE(db.exec("CREATE TABLE t (v INTEGER)"));
    {
        auto tx = Transaction::begin(db);
        ASSERT_TRUE(tx.has_value());
        ASSERT_TRUE(db.exec("INSERT INTO t VALUES (1)"));
        ASSERT_TRUE(tx->commit());
        EXPECT_FALSE(tx->commit().has_value()) << "already finished";
    }
    EXPECT_EQ(count(db, "t"), 1);
}

TEST(Transaction, DestructorRollsBack) {
    Database db = memory();
    ASSERT_TRUE(db.exec("CREATE TABLE t (v INTEGER)"));
    {
        auto tx = Transaction::begin(db);
        ASSERT_TRUE(db.exec("INSERT INTO t VALUES (1)"));
    }
    EXPECT_EQ(count(db, "t"), 0);
}

TEST(Migrate, AppliesEachVersionOncePerComponent) {
    Database db = memory();
    const std::array<Migration, 2> file{{{1, "CREATE TABLE file_a (v INTEGER);"}, {2, "INSERT INTO file_a VALUES (7);"}}};
    ASSERT_TRUE(hoardor::db::migrate(db, "file", file));
    ASSERT_TRUE(hoardor::db::migrate(db, "file", file)) << "running again is a no-op";
    EXPECT_EQ(count(db, "file_a"), 1);

    const std::array<Migration, 1> audio{{{1, "CREATE TABLE audio_a (v INTEGER);"}}};
    ASSERT_TRUE(hoardor::db::migrate(db, "audio", audio)) << "versions are per component";
    EXPECT_EQ(count(db, "audio_a"), 0);
}

TEST(Migrate, FailedMigrationRollsBackAndKeepsVersion) {
    Database db = memory();
    const std::array<Migration, 1> v1{{{1, "CREATE TABLE file_a (v INTEGER);"}}};
    ASSERT_TRUE(hoardor::db::migrate(db, "file", v1));
    const std::array<Migration, 2> v2{{{1, "CREATE TABLE file_a (v INTEGER);"},
                                       {2, "INSERT INTO file_a VALUES (1); INSERT INTO nope VALUES (1);"}}};
    auto r = hoardor::db::migrate(db, "file", v2);
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().message.find("file migration 2"), std::string::npos);
    EXPECT_EQ(count(db, "file_a"), 0) << "the half-applied migration was rolled back";
    auto st = db.prepare("SELECT version FROM db_migrations WHERE component = 'file'");
    ASSERT_TRUE(st->step().value());
    EXPECT_EQ(st->column_int64(0), 1);
}

TEST(Wal, ReaderSeesCommittedDataWhileAnotherConnectionWrites) {
    hoardor::test::TempDir dir;
    auto writer = Database::open(dir.path() / "library.db");
    auto reader = Database::open(dir.path() / "library.db");
    ASSERT_TRUE(writer && reader);
    ASSERT_TRUE(writer->exec("CREATE TABLE t (v INTEGER); INSERT INTO t VALUES (1);"));

    auto tx = Transaction::begin(*writer);
    ASSERT_TRUE(tx.has_value());
    ASSERT_TRUE(writer->exec("INSERT INTO t VALUES (2)"));
    EXPECT_EQ(count(*reader, "t"), 1) << "the reader isn't blocked and sees only committed rows";
    ASSERT_TRUE(tx->commit());
    EXPECT_EQ(count(*reader, "t"), 2);
}
