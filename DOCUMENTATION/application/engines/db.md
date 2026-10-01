# DB engine (`hoardor::db`)

Status: **Built** (2026-10-01) as part of File Sync v1, phase 2. 12 tests in `tests/db/database_test.cpp`. The default page cache (2 MB) is kept: 8 and 16 MB made no measurable difference in the sync benchmark.

`db` is infrastructure, not a feature engine (ARCHITECTURE §3). It provides the *mechanics* of SQLite. Every engine owns its own tables, migrations, and queries.

## 1. Responsibilities

- **Connections:** open a database file (or an in-memory one for tests) with hoardor's pragmas.
- **Statements:** prepare, bind, step, and read columns, with RAII cleanup.
- **Transactions:** RAII `BEGIN IMMEDIATE` / `COMMIT`, rolling back automatically if not committed.
- **Migrations:** each engine registers ordered SQL migrations under its component name. `db` records the applied version per component and runs each migration exactly once, in a transaction.

Not in scope: query builders, ORMs, a statement cache class, and a writer thread. Repositories keep their own prepared statements as members while they need them.

## 2. Public API (`include/hoardor/db/database.hpp`)

```cpp
namespace hoardor::db {

struct Error { int code = 0; std::string message; };   // SQLite result code + message
template <class T> using Result = std::expected<T, Error>;

struct Options { int busy_timeout_ms = 5000; };

class Statement {            // move-only, wraps sqlite3_stmt
public:
    Statement& bind(int index, std::int64_t value);     // 1-based, like SQLite
    Statement& bind(int index, double value);
    Statement& bind(int index, std::string_view text);  // copied by SQLite
    Statement& bind_null(int index);
    Result<bool> step();     // true: a row is ready; false: done. Reports any earlier bind error
    Result<void> run();      // step to completion (writes)
    void reset();            // ready to bind and run again
    std::int64_t column_int64(int column) const;        // 0-based, like SQLite
    double column_double(int column) const;
    std::string column_text(int column) const;
    bool column_is_null(int column) const;
};

class Database {             // move-only, one connection, used by one thread at a time
public:
    static Result<Database> open(const std::filesystem::path& file, Options options = {});
    static Result<Database> open_in_memory(Options options = {});
    Result<void> exec(std::string_view sql);            // one or more statements, no results
    Result<Statement> prepare(std::string_view sql);
    std::int64_t last_insert_id() const;
    std::int64_t changes() const;                       // rows changed by the last statement
};

class Transaction {          // move-only
public:
    static Result<Transaction> begin(Database& database);   // BEGIN IMMEDIATE
    Result<void> commit();
    ~Transaction();          // rolls back unless committed
};

struct Migration { int version; std::string_view sql; };
Result<void> migrate(Database& database, std::string_view component, std::span<const Migration> migrations);

}
```

`sqlite3.h` isn't part of the public API: the header only forward-declares `sqlite3` and `sqlite3_stmt`, so code that includes hoardor never sees SQLite's macros.

## 3. Behavior

- **Pragmas on open (files):**
  - `journal_mode=WAL`: readers aren't blocked by a writer.
  - `synchronous=NORMAL`: safe with WAL, and much faster than `FULL`.
  - `foreign_keys=ON`.
  - `busy_timeout` from `Options`.

  In-memory databases skip WAL, which doesn't apply to them.
- **`BEGIN IMMEDIATE`** takes the write lock at the start of the transaction. Two writers therefore wait on `busy_timeout` instead of deadlocking halfway through.
- **The migrations table** is `db_migrations(component TEXT PRIMARY KEY, version INTEGER NOT NULL)`. Each migration with a version above the stored one runs in its own transaction, together with the version bump, so a failing migration leaves the previous version intact.
- **Threads:** one `Database` per thread (ARCHITECTURE §3). SQLite is built in its default serialized mode, so moving a connection to another thread before using it there is safe.
- **Paths** are passed to SQLite as UTF-8.

## 4. Dependency

- **SQLite:** the 3.46.1 amalgamation (one C file), fetched with CMake `FetchContent` and compiled into a static library `hoardor_sqlite3`.
- **Compile options:** `SQLITE_DQS=0` (no double-quoted string literals), `SQLITE_DEFAULT_MEMSTATUS=0`, `SQLITE_OMIT_LOAD_EXTENSION`.
- **Why the amalgamation:** it builds identically on Windows, Linux, and macOS with no system package. That matters for Windows, the user's main OS.

## 5. Tests

`tests/db/database_test.cpp`:
- opening in memory, opening a file (and checking WAL is on), opening a path that can't be created
- `exec` and `prepare` errors
- binding and reading int64, double, text (including UTF-8 and embedded NUL), and NULL
- `step` and `run`, plus `reset` and reuse
- transaction commit, and rollback by destructor
- migrations: applied once, versioned per component, a failing migration rolled back and reported
- WAL: a second connection reads committed data while the first holds a write transaction
