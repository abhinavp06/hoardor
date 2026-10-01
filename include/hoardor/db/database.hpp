#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

// Forward declarations keep sqlite3.h (and its macros) out of hoardor's public headers.
struct sqlite3;
struct sqlite3_stmt;

namespace hoardor::db {

struct Error {
    int code = 0;  // SQLite result code
    std::string message;
};

template <class T>
using Result = std::expected<T, Error>;

struct Options {
    int busy_timeout_ms = 5000;
};

// One prepared statement. Move-only; finalized on destruction.
class Statement {
public:
    Statement(Statement&& other) noexcept;
    Statement& operator=(Statement&& other) noexcept;
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    ~Statement();

    // Parameter indexes are 1-based, like SQLite. A bind error is reported by the next step().
    Statement& bind(int index, std::int64_t value);
    Statement& bind(int index, int value) { return bind(index, static_cast<std::int64_t>(value)); }
    Statement& bind(int index, double value);
    Statement& bind(int index, std::string_view text);
    Statement& bind_null(int index);

    // true: a row is ready to read; false: the statement is done.
    Result<bool> step();
    // Steps to completion, for statements that don't return rows.
    Result<void> run();
    // Makes the statement ready to bind and run again.
    void reset();

    // Column indexes are 0-based, like SQLite.
    std::int64_t column_int64(int column) const;
    double column_double(int column) const;
    std::string column_text(int column) const;
    bool column_is_null(int column) const;

private:
    friend class Database;
    Statement(sqlite3* db, sqlite3_stmt* stmt) : db_(db), stmt_(stmt) {}
    void note_bind(int rc);

    sqlite3* db_ = nullptr;
    sqlite3_stmt* stmt_ = nullptr;
    int bind_error_ = 0;
};

// One SQLite connection. Move-only. Use each Database from one thread at a time
// (ARCHITECTURE §3: one connection per thread).
class Database {
public:
    static Result<Database> open(const std::filesystem::path& file, Options options = {});
    static Result<Database> open_in_memory(Options options = {});

    Database(Database&& other) noexcept;
    Database& operator=(Database&& other) noexcept;
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    ~Database();

    // Runs one or more statements that return no rows.
    Result<void> exec(std::string_view sql);
    Result<Statement> prepare(std::string_view sql);
    std::int64_t last_insert_id() const;
    std::int64_t changes() const;

private:
    explicit Database(sqlite3* db) : db_(db) {}
    static Result<Database> open_uri(const std::string& name, int flags, bool wal, Options options);

    sqlite3* db_ = nullptr;
};

// BEGIN IMMEDIATE ... COMMIT. Rolls back on destruction unless committed.
class Transaction {
public:
    static Result<Transaction> begin(Database& database);

    Transaction(Transaction&& other) noexcept;
    Transaction& operator=(Transaction&&) = delete;
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    ~Transaction();

    Result<void> commit();

private:
    explicit Transaction(Database& database) : database_(&database) {}
    Database* database_ = nullptr;  // null once committed or moved from
};

struct Migration {
    int version;  // 1, 2, 3, ... in order
    std::string_view sql;
};

// Applies the migrations of one component (an engine name, e.g. "file") whose
// version is above the recorded one. Each runs in its own transaction.
Result<void> migrate(Database& database, std::string_view component, std::span<const Migration> migrations);

}
